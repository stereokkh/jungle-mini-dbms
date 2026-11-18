// SQL 실행기(query_run) 단위 테스트. 임시 폴더에 users 테이블을 만들고 HTTP 없이 바로 부른다.

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "query.h"
#include "rpc.h"

static Db db;
static char dir[64];

// [역할] 임시 폴더를 만들고 users 스키마(id, name, age)를 넣은 뒤 DB 를 연다.
static void setup(void)
{
    char path[128];
    DbError err;

    strcpy(dir, "/tmp/dbms-query-test-XXXXXX");
    assert(mkdtemp(dir) != NULL);
    snprintf(path, sizeof path, "%s/users.schema", dir);
    FILE *fp = fopen(path, "w");
    assert(fp != NULL);
    fputs("id,INT\nname,TEXT\nage,INT\n", fp);
    fclose(fp);
    snprintf(path, sizeof path, "%s/items.schema", dir);   // 동시성 테스트에서 처음 열 테이블
    fp = fopen(path, "w");
    assert(fp != NULL);
    fputs("id,INT\nname,TEXT\n", fp);
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

// [역할] sql 이 성공하고 결과 JSON 이 json 과 같은지 확인한다.
static void expect_ok(const char *sql, const char *json)
{
    QueryError qe;
    char *out = query_run(&db, sql, &qe);
    if (out == NULL || strcmp(out, json) != 0) {
        fprintf(stderr, "SQL:  %s\nGOT:  %s\nWANT: %s\n", sql, out ? out : qe.message, json);
        abort();
    }
    free(out);
}

// [역할] sql 이 실패하고 오류 코드·실행된 문장 수가 기대와 같은지 확인한다.
static void expect_fail(const char *sql, int code, int executed)
{
    QueryError qe;
    char *out = query_run(&db, sql, &qe);
    if (out != NULL || qe.code != code || qe.executed != executed) {
        fprintf(stderr, "SQL: %s\nGOT: %s code=%d executed=%d\n", sql, out ? out : "(fail)",
                qe.code, qe.executed);
        abort();
    }
}

// INSERT 는 id 를 붙여 주고, SELECT 는 컬럼 이름과 행을 돌려준다
static void test_insert_and_select(void)
{
    expect_ok("INSERT INTO users VALUES ('Kim', 20);",
              "{\"results\":[{\"type\":\"insert\",\"id\":1}]}");
    expect_ok("INSERT INTO users VALUES ('Lee', 30); INSERT INTO users VALUES ('Park', 40);",
              "{\"results\":[{\"type\":\"insert\",\"id\":2},{\"type\":\"insert\",\"id\":3}]}");
    expect_ok("SELECT * FROM users WHERE id = 2;",
              "{\"results\":[{\"type\":\"select\",\"rows\":[[2,\"Lee\",30]],"
              "\"columns\":[\"id\",\"name\",\"age\"],\"row_count\":1}]}");
    expect_ok("SELECT name FROM users WHERE age > 25",
              "{\"results\":[{\"type\":\"select\",\"rows\":[[\"Lee\"],[\"Park\"]],"
              "\"columns\":[\"name\"],\"row_count\":2}]}");
    expect_ok("SELECT * FROM users WHERE id = 999;",
              "{\"results\":[{\"type\":\"select\",\"rows\":[],"
              "\"columns\":[\"id\",\"name\",\"age\"],\"row_count\":0}]}");
}

// 문자열 안의 " 와 \ 는 JSON 에서 이스케이프된다
static void test_json_escape(void)
{
    expect_ok("INSERT INTO users VALUES ('say \"hi\" \\', 1);",
              "{\"results\":[{\"type\":\"insert\",\"id\":4}]}");
    expect_ok("SELECT name FROM users WHERE id = 4;",
              "{\"results\":[{\"type\":\"select\",\"rows\":[[\"say \\\"hi\\\" \\\\\"]],"
              "\"columns\":[\"name\"],\"row_count\":1}]}");
}

// 엔진 오류는 JSON-RPC 오류 코드로 바뀐다. executed = 오류 전에 실행된 문장 수
static void test_errors(void)
{
    expect_fail("SELEC * FROM users;", RPC_SQL_ERROR, 0);
    expect_fail("SELECT * FROM nope;", RPC_TABLE_NOT_FOUND, 0);
    expect_fail("INSERT INTO users VALUES ('Choi', 50); SELECT bad FROM users;", RPC_SQL_ERROR, 1);
    expect_fail("  ;  ", RPC_INVALID_PARAMS, 0);
}

// [역할] table 의 행 수를 SELECT 로 센다.
static int count_rows(const char *table)
{
    char sql[64];
    QueryError qe;
    snprintf(sql, sizeof sql, "SELECT id FROM %s;", table);
    char *out = query_run(&db, sql, &qe);
    assert(out != NULL);
    const char *p = strstr(out, "\"row_count\":");
    assert(p != NULL);
    int n = atoi(p + 12);
    free(out);
    return n;
}

#define THREADS     8
#define PER_THREAD 50

// [역할] 스레드 하나가 할 일: users 에 INSERT + SELECT, items(아직 안 연 테이블)에 INSERT 를 반복.
static void *hammer(void *arg)
{
    (void)arg;
    for (int i = 0; i < PER_THREAD; i++) {
        QueryError qe;
        char *out = query_run(&db, "INSERT INTO users VALUES ('T', 1); "
                                   "SELECT * FROM users WHERE id = 1; "
                                   "INSERT INTO items VALUES ('x');", &qe);
        if (out == NULL) {
            fprintf(stderr, "thread query failed: %s\n", qe.message);
            abort();
        }
        free(out);
    }
    return NULL;
}

// 3단계: 여러 스레드가 동시에 불러도 행이 사라지거나 id 가 겹치지 않는다
static void test_concurrent(void)
{
    int before = count_rows("users");
    pthread_t th[THREADS];

    for (int i = 0; i < THREADS; i++) assert(pthread_create(&th[i], NULL, hammer, NULL) == 0);
    for (int i = 0; i < THREADS; i++) pthread_join(th[i], NULL);

    assert(count_rows("users") == before + THREADS * PER_THREAD);
    assert(count_rows("items") == THREADS * PER_THREAD);
    // id 가 겹치지 않았다면 마지막 id 는 정확히 THREADS * PER_THREAD
    char want[128];
    snprintf(want, sizeof want, "{\"results\":[{\"type\":\"select\",\"rows\":[[%d]],"
             "\"columns\":[\"id\"],\"row_count\":1}]}", THREADS * PER_THREAD);
    char sql[64];
    snprintf(sql, sizeof sql, "SELECT id FROM items WHERE id = %d;", THREADS * PER_THREAD);
    expect_ok(sql, want);
}

// [역할] 테스트 함수를 차례로 실행한다. assert 하나라도 실패하면 멈춘다.
int main(void)
{
    setup();
    test_insert_and_select();
    test_json_escape();
    test_errors();
    test_concurrent();
    teardown();
    printf("test_query: ok\n");
    return 0;
}
