// SQL API(query_run) 단위 테스트. 임시 폴더에 users 테이블을 만들고 HTTP 없이 바로 부른다.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "query.h"

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

// [역할] sql 을 실행해 상태 코드와 JSON 을 비교한다.
static void expect(const char *sql, int status, const char *json)
{
    int st = 0;
    char *out = query_run(&db, sql, &st);
    assert(out != NULL);
    if (st != status || strcmp(out, json) != 0) {
        fprintf(stderr, "SQL:  %s\nGOT:  %d %s\nWANT: %d %s\n", sql, st, out, status, json);
        abort();
    }
    free(out);
}

// INSERT 는 id 를 붙여 주고, SELECT 는 컬럼 이름과 행을 돌려준다
static void test_insert_and_select(void)
{
    expect("INSERT INTO users VALUES ('Kim', 20);", 200,
           "{\"ok\":true,\"results\":[{\"type\":\"insert\",\"id\":1}]}");
    expect("INSERT INTO users VALUES ('Lee', 30); INSERT INTO users VALUES ('Park', 40);", 200,
           "{\"ok\":true,\"results\":[{\"type\":\"insert\",\"id\":2},{\"type\":\"insert\",\"id\":3}]}");
    expect("SELECT * FROM users WHERE id = 2;", 200,
           "{\"ok\":true,\"results\":[{\"type\":\"select\",\"rows\":[[2,\"Lee\",30]],"
           "\"columns\":[\"id\",\"name\",\"age\"],\"row_count\":1}]}");
    expect("SELECT name FROM users WHERE age > 25", 200,
           "{\"ok\":true,\"results\":[{\"type\":\"select\",\"rows\":[[\"Lee\"],[\"Park\"]],"
           "\"columns\":[\"name\"],\"row_count\":2}]}");
    expect("SELECT * FROM users WHERE id = 999;", 200,
           "{\"ok\":true,\"results\":[{\"type\":\"select\",\"rows\":[],"
           "\"columns\":[\"id\",\"name\",\"age\"],\"row_count\":0}]}");
}

// 문자열 안의 " 와 \ 는 JSON 에서 이스케이프된다
static void test_json_escape(void)
{
    expect("INSERT INTO users VALUES ('say \"hi\" \\', 1);", 200,
           "{\"ok\":true,\"results\":[{\"type\":\"insert\",\"id\":4}]}");
    expect("SELECT name FROM users WHERE id = 4;", 200,
           "{\"ok\":true,\"results\":[{\"type\":\"select\",\"rows\":[[\"say \\\"hi\\\" \\\\\"]],"
           "\"columns\":[\"name\"],\"row_count\":1}]}");
}

// 엔진 오류는 HTTP 상태 코드로 바뀐다. executed = 오류 전에 실행된 문장 수
static void test_errors(void)
{
    int st;
    char *out;

    out = query_run(&db, "SELEC * FROM users;", &st);
    assert(st == 400 && strstr(out, "\"ok\":false") && strstr(out, "\"executed\":0"));
    free(out);

    out = query_run(&db, "SELECT * FROM nope;", &st);
    assert(st == 404);
    free(out);

    out = query_run(&db, "INSERT INTO users VALUES ('Choi', 50); SELECT bad FROM users;", &st);
    assert(st == 400 && strstr(out, "\"executed\":1"));   // 첫 INSERT 는 이미 반영됨
    free(out);

    out = query_run(&db, "  ;  ", &st);
    assert(st == 400 && strstr(out, "no SQL statement"));
    free(out);
}

// [역할] 테스트 함수를 차례로 실행한다. assert 하나라도 실패하면 멈춘다.
int main(void)
{
    setup();
    test_insert_and_select();
    test_json_escape();
    test_errors();
    teardown();
    printf("test_query: ok\n");
    return 0;
}
