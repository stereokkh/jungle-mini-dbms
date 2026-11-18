// JSON 읽기(json.c)와 JSON-RPC 처리기(rpc.c) 단위 테스트. HTTP 없이 본문 문자열만 넣는다.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "json.h"
#include "rpc.h"

static Db db;
static char dir[64];

// [역할] 임시 폴더에 users 테이블을 만들고 DB 를 연다.
static void setup(void)
{
    char path[128];
    DbError err;

    strcpy(dir, "/tmp/dbms-rpc-test-XXXXXX");
    assert(mkdtemp(dir) != NULL);
    snprintf(path, sizeof path, "%s/users.schema", dir);
    FILE *fp = fopen(path, "w");
    assert(fp != NULL);
    fputs("id,INT\nname,TEXT\nage,INT\n", fp);
    fclose(fp);
    assert(db_open(dir, &db, &err) == DB_OK);
}

// [역할] DB 를 닫고 임시 폴더를 지운다.
static void teardown(void)
{
    char cmd[128];
    db_close(&db);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    assert(system(cmd) == 0);
}

// [역할] 요청 본문 하나를 rpc_handle 에 넣고 응답이 기대와 같은지 본다.
static void expect(const char *body, const char *want)
{
    int notification = 0;
    char *got = rpc_handle(&db, body, strlen(body), &notification);
    if (got == NULL || strcmp(got, want) != 0) {
        fprintf(stderr, "REQ:  %s\nGOT:  %s\nWANT: %s\n", body, got ? got : "(null)", want);
        abort();
    }
    assert(notification == 0);
    free(got);
}

// [역할] 응답에 needle 이 들어 있는지만 본다 (오류 메시지 전체를 맞추기 번거로울 때).
static void expect_contains(const char *body, const char *needle)
{
    int notification = 0;
    char *got = rpc_handle(&db, body, strlen(body), &notification);
    if (got == NULL || strstr(got, needle) == NULL) {
        fprintf(stderr, "REQ:  %s\nGOT:  %s\nWANT: ...%s...\n", body, got ? got : "(null)", needle);
        abort();
    }
    free(got);
}

// json.c: 이스케이프와 \uXXXX(한글, 이모지)를 풀어 읽는다
static void test_json_reader(void)
{
    const char *text = "  \"a\\\"b\\n\\u00e9\\uac00\\ud83d\\ude00\" ";
    JsonReader r = { text, text + strlen(text) };
    char *s = NULL;
    assert(jr_read_string(&r, &s) == 0);
    assert(strcmp(s, "a\"b\n\xc3\xa9\xea\xb0\x80\xf0\x9f\x98\x80") == 0);   // é 가 😀
    free(s);

    const char *bad[] = { "{", "[1,]", "{\"a\" 1}", "\"\\x\"", "01x", "tru", "\"\\u0000\"" };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        JsonReader b = { bad[i], bad[i] + strlen(bad[i]) };
        int ok = jr_skip_value(&b) == 0 && (jr_skip_ws(&b), b.p == b.end);
        assert(!ok);
    }
    const char *good = "{\"a\":[1,-2.5e3,true,false,null,{}],\"b\":\"x\"}";
    JsonReader g = { good, good + strlen(good) };
    assert(jr_skip_value(&g) == 0 && g.p == g.end);
}

// 정상 호출: query (이름 방식·위치 방식), ping, 문자열 id
static void test_success(void)
{
    expect("{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":1}",
           "{\"jsonrpc\":\"2.0\",\"result\":\"pong\",\"id\":1}");
    expect("{\"jsonrpc\":\"2.0\",\"method\":\"query\","
           "\"params\":{\"sql\":\"INSERT INTO users VALUES ('Kim', 20);\"},\"id\":\"a-1\"}",
           "{\"jsonrpc\":\"2.0\",\"result\":{\"results\":[{\"type\":\"insert\",\"id\":1}]},\"id\":\"a-1\"}");
    expect("{\"id\":7,\"params\":[\"SELECT name FROM users WHERE id = 1;\"],"
           "\"method\":\"query\",\"jsonrpc\":\"2.0\"}",   // 키 순서는 상관없다
           "{\"jsonrpc\":\"2.0\",\"result\":{\"results\":[{\"type\":\"select\",\"rows\":[[\"Kim\"]],"
           "\"columns\":[\"name\"],\"row_count\":1}]},\"id\":7}");
}

// 규격 오류 코드: -32700, -32600, -32601, -32602
static void test_protocol_errors(void)
{
    expect("{not json",
           "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32700,"
           "\"message\":\"parse error: body is not valid JSON\"},\"id\":null}");
    expect_contains("", "\"code\":-32700");
    expect_contains("[{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":1}]", "\"code\":-32600");
    expect_contains("\"hello\"", "\"code\":-32600");
    expect_contains("{\"method\":\"ping\",\"id\":1}", "\"code\":-32600");                // jsonrpc 없음
    expect_contains("{\"jsonrpc\":\"1.0\",\"method\":\"ping\",\"id\":1}", "\"code\":-32600");
    expect_contains("{\"jsonrpc\":\"2.0\",\"method\":\"ping\",\"id\":[1]}", "\"code\":-32600");
    expect_contains("{\"jsonrpc\":\"2.0\",\"method\":\"drop\",\"id\":3}", "\"code\":-32601");
    expect_contains("{\"jsonrpc\":\"2.0\",\"method\":\"query\",\"id\":4}", "\"code\":-32602");  // params 없음
    expect_contains("{\"jsonrpc\":\"2.0\",\"method\":\"query\",\"params\":{\"sql\":1},\"id\":5}",
                    "\"code\":-32602");
}

// SQL 오류는 서버 정의 코드 + data.executed
static void test_sql_errors(void)
{
    expect_contains("{\"jsonrpc\":\"2.0\",\"method\":\"query\",\"params\":[\"SELEC 1\"],\"id\":1}",
                    "\"code\":-32001");
    expect_contains("{\"jsonrpc\":\"2.0\",\"method\":\"query\",\"params\":[\"SELECT * FROM nope;\"],\"id\":1}",
                    "\"code\":-32002");
    expect_contains("{\"jsonrpc\":\"2.0\",\"method\":\"query\","
                    "\"params\":[\"INSERT INTO users VALUES ('Lee', 1); SELECT x FROM users;\"],\"id\":1}",
                    "\"data\":{\"executed\":1}");
}

// id 가 없으면 알림: 실행은 하지만 응답은 없다
static void test_notification(void)
{
    const char *body = "{\"jsonrpc\":\"2.0\",\"method\":\"query\","
                       "\"params\":[\"INSERT INTO users VALUES ('Quiet', 1);\"]}";
    int notification = 0;
    char *got = rpc_handle(&db, body, strlen(body), &notification);
    assert(got == NULL && notification == 1);

    // 정말 실행됐는지 확인
    expect_contains("{\"jsonrpc\":\"2.0\",\"method\":\"query\","
                    "\"params\":[\"SELECT name FROM users WHERE name = 'Quiet';\"],\"id\":9}",
                    "[\"Quiet\"]");
}

// [역할] 테스트 함수를 차례로 실행한다. assert 하나라도 실패하면 멈춘다.
int main(void)
{
    setup();
    test_json_reader();
    test_success();
    test_protocol_errors();
    test_sql_errors();
    test_notification();
    teardown();
    printf("test_rpc: ok\n");
    return 0;
}
