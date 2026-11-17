#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db.h"
#include "schema.h"
#include "storage.h"

#define USERS_SCHEMA "id,INT\nname,TEXT\nage,INT\n"

// 테스트마다 새 임시 디렉터리를 쓴다. 실제 data/ 를 건드리지 않고, 테스트끼리도 섞이지 않는다.
static void make_dir(char *dir, size_t size)
{
    snprintf(dir, size, "/tmp/mini-dbms-test-XXXXXX");
    assert(mkdtemp(dir) != NULL);
}

static void remove_dir(const char *dir)
{
    char cmd[300];

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    assert(system(cmd) == 0);
}

static void write_file(const char *dir, const char *name, const char *content)
{
    char path[300];

    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *fp = fopen(path, "wb");
    assert(fp != NULL);
    fputs(content, fp);
    fclose(fp);
}

// 스키마 파일 하나를 써 놓고 schema_load 의 결과만 돌려준다.
static DbStatus load_schema(const char *content, Schema *s, DbError *err)
{
    char dir[64], path[300];

    make_dir(dir, sizeof dir);
    write_file(dir, "t.schema", content);
    snprintf(path, sizeof path, "%s/t.schema", dir);
    DbStatus st = schema_load(path, s, err);
    remove_dir(dir);
    return st;
}

// 읽은 행을 모아 두는 콜백. stop_after 개를 모으면 읽기를 멈추라고 알린다.
typedef struct {
    Value rows[8][3];
    long  offsets[8];
    int   count;
    int   stop_after;
} Collected;

static int collect(const Value *fields, int field_count, long offset, void *ctx)
{
    Collected *c = ctx;

    assert(field_count == 3 && c->count < 8);
    memcpy(c->rows[c->count], fields, sizeof c->rows[0]);
    c->offsets[c->count] = offset;
    c->count++;
    return c->count == c->stop_after;
}

static void test_schema_load(void)
{
    Schema s;
    DbError err;

    assert(load_schema(USERS_SCHEMA, &s, &err) == DB_OK);
    assert(s.count == 3);
    assert(strcmp(s.cols[0].name, "id") == 0 && s.cols[0].type == COL_INT);
    assert(strcmp(s.cols[1].name, "name") == 0 && s.cols[1].type == COL_TEXT);
    assert(strcmp(s.cols[2].name, "age") == 0 && s.cols[2].type == COL_INT);

    // Windows 줄 끝, 마지막 줄바꿈 없음, 빈 줄
    assert(load_schema("id,INT\r\nname,TEXT\r\n\r\nage,INT", &s, &err) == DB_OK);
    assert(s.count == 3 && strcmp(s.cols[1].name, "name") == 0);
}

static void test_schema_errors(void)
{
    Schema s;
    DbError err;
    char big[2048];

    assert(schema_load("/tmp/mini-dbms-no-such-dir/t.schema", &s, &err) == DB_ERR_NO_TABLE);
    assert(err.code == DB_ERR_NO_TABLE && err.pos == -1);

    assert(load_schema("", &s, &err) == DB_ERR_SCHEMA);
    assert(load_schema("name,TEXT\nid,INT\n", &s, &err) == DB_ERR_SCHEMA);
    assert(load_schema("id,TEXT\n", &s, &err) == DB_ERR_SCHEMA);
    assert(load_schema("id,INT\nname\n", &s, &err) == DB_ERR_SCHEMA);
    assert(load_schema("id,INT\n,TEXT\n", &s, &err) == DB_ERR_SCHEMA);
    assert(load_schema("id,INT\nname,text\n", &s, &err) == DB_ERR_SCHEMA);
    assert(load_schema("id,INT\nname,FLOAT\n", &s, &err) == DB_ERR_SCHEMA);

    // 컬럼 16개는 되고 17개는 안 된다
    strcpy(big, "id,INT\n");
    for (int i = 0; i < 15; i++) {
        strcat(big, "c,INT\n");
    }
    assert(load_schema(big, &s, &err) == DB_OK && s.count == 16);
    strcat(big, "c,INT\n");
    assert(load_schema(big, &s, &err) == DB_ERR_TOO_LONG);

    // 64바이트 이름, 버퍼보다 긴 줄
    strcpy(big, "id,INT\n");
    memset(big + 7, 'n', 64);
    strcpy(big + 71, ",INT\n");
    assert(load_schema(big, &s, &err) == DB_ERR_TOO_LONG);
    memset(big + 7, 'n', 500);
    strcpy(big + 507, ",INT\n");
    assert(load_schema(big, &s, &err) == DB_ERR_TOO_LONG);
}

// 통과 기준: 빈 테이블의 next_id 는 1, offset 은 0 과 15, 다시 열면 next_id 가 이어진다
static void test_append_and_reopen(void)
{
    char dir[64];
    Db db;
    Table *t, *again;
    DbError err;
    long offset = -1;
    Collected c = {0};

    make_dir(dir, sizeof dir);
    write_file(dir, "users.schema", USERS_SCHEMA);

    assert(db_open(dir, &db, &err) == DB_OK);
    assert(db_get_table(&db, "users", &t, &err) == DB_OK);
    assert(t->next_id == 1);
    assert(t->schema.count == 3);

    assert(storage_append(t, "1,\"Kim, Jr\",20", &offset, &err) == DB_OK);
    assert(offset == 0);
    assert(storage_append(t, "2,\"Lee\",31", &offset, &err) == DB_OK);
    assert(offset == 15);

    // 같은 테이블을 다시 달라고 하면 새로 열지 않고 같은 것을 준다
    assert(db_get_table(&db, "users", &again, &err) == DB_OK);
    assert(again == t && db.table_count == 1);

    // 방금 넣은 줄이 (아직 닫지 않았어도) 읽힌다
    assert(storage_scan(&db, t, collect, &c, &err) == DB_OK);
    assert(c.count == 2);
    assert(c.rows[0][0].type == VAL_INT && c.rows[0][0].i == 1);
    assert(c.rows[0][1].type == VAL_STR && strcmp(c.rows[0][1].s, "Kim, Jr") == 0);
    assert(c.rows[0][2].type == VAL_INT && c.rows[0][2].i == 20);
    assert(strcmp(c.rows[1][1].s, "Lee") == 0);
    assert(c.offsets[0] == 0 && c.offsets[1] == 15);
    db_close(&db);

    assert(db_open(dir, &db, &err) == DB_OK);
    assert(db_get_table(&db, "users", &t, &err) == DB_OK);
    assert(t->next_id == 3);
    assert(storage_append(t, "3,\"Park\",5", &offset, &err) == DB_OK);
    assert(offset == 26);
    db_close(&db);
    remove_dir(dir);
}

static void test_scan_values(void)
{
    char dir[64];
    Db db;
    Table *t;
    DbError err;
    Collected c = {0};

    make_dir(dir, sizeof dir);
    write_file(dir, "users.schema", USERS_SCHEMA);
    // id 가 순서대로가 아니어도 next_id 는 가장 큰 id + 1 이다.
    write_file(dir, "users.csv",
               "7,\"He said \"\"hi\"\"\",-5\n"
               "2,\"\",0\n"
               "3,\"김건호\",30\n");

    assert(db_open(dir, &db, &err) == DB_OK);
    assert(db_get_table(&db, "users", &t, &err) == DB_OK);
    assert(t->next_id == 8);

    assert(storage_scan(&db, t, collect, &c, &err) == DB_OK);
    assert(c.count == 3);
    assert(strcmp(c.rows[0][1].s, "He said \"hi\"") == 0 && c.rows[0][2].i == -5);
    assert(strcmp(c.rows[1][1].s, "") == 0);
    assert(strcmp(c.rows[2][1].s, "김건호") == 0);
    assert(c.offsets[1] == 22 && c.offsets[2] == 29);

    // 콜백이 0 이 아닌 값을 돌려주면 거기서 멈춘다
    memset(&c, 0, sizeof c);
    c.stop_after = 1;
    assert(storage_scan(&db, t, collect, &c, &err) == DB_OK);
    assert(c.count == 1);

    // 데이터 파일이 없으면 행이 0개다
    char path[300];
    snprintf(path, sizeof path, "%s/users.csv", dir);
    assert(unlink(path) == 0);
    memset(&c, 0, sizeof c);
    assert(storage_scan(&db, t, collect, &c, &err) == DB_OK);
    assert(c.count == 0);

    db_close(&db);
    remove_dir(dir);
}

// 데이터 파일 내용을 content 로 써 놓고 테이블을 열 때의 결과를 돌려준다.
static DbStatus open_with_data(const char *content, DbError *err)
{
    char dir[64];
    Db db;
    Table *t;

    make_dir(dir, sizeof dir);
    write_file(dir, "users.schema", USERS_SCHEMA);
    write_file(dir, "users.csv", content);
    assert(db_open(dir, &db, err) == DB_OK);
    DbStatus st = db_get_table(&db, "users", &t, err);
    assert(st == DB_OK || db.table_count == 0);  // 실패하면 열린 테이블로 남지 않는다
    db_close(&db);
    remove_dir(dir);
    return st;
}

static void test_corrupt_data(void)
{
    DbError err;
    char big[10000];

    assert(open_with_data("1,\"Kim\",20\n", &err) == DB_OK);
    assert(open_with_data("abc\n", &err) == DB_ERR_IO);
    assert(open_with_data("1,\"Kim\"\n", &err) == DB_ERR_IO);          // 필드 부족
    assert(open_with_data("1,\"Kim\",20,4\n", &err) == DB_ERR_IO);     // 필드 초과
    assert(open_with_data("1,5,20\n", &err) == DB_ERR_IO);             // 타입이 스키마와 다름
    assert(open_with_data("1,\"Kim,20\n", &err) == DB_ERR_IO);         // 닫히지 않은 따옴표
    assert(open_with_data("1,\"Kim\"x,20\n", &err) == DB_ERR_IO);
    assert(open_with_data("1,\"Kim\",20\n\n", &err) == DB_ERR_IO);     // 빈 줄
    assert(open_with_data("1,\"Kim\",99999999999999999999\n", &err) == DB_ERR_IO);
    assert(strstr(err.message, "users") != NULL);

    // 줄바꿈 없이 끝난 마지막 줄은 쓰다가 끊긴 것으로 본다
    assert(open_with_data("1,\"Kim\",20\n2,\"Lee\",31", &err) == DB_ERR_IO);
    assert(strstr(err.message, "incomplete row at offset 11") != NULL);

    // id 가 long long 의 끝이면 다음 id 를 만들 수 없다 (+1 이 넘친다)
    assert(open_with_data("9223372036854775807,\"Kim\",20\n", &err) == DB_ERR_IO);
    assert(open_with_data("9223372036854775806,\"Kim\",20\n", &err) == DB_OK);

    // 한도(8192바이트)를 넘는 줄
    strcpy(big, "1,\"");
    memset(big + 3, 'a', 9000);
    strcpy(big + 9003, "\",20\n");
    assert(open_with_data(big, &err) == DB_ERR_TOO_LONG);
}

static void test_db_errors(void)
{
    char dir[64], name[100], long_dir[400];
    Db db;
    Table *t;
    DbError err;
    long offset = -1;
    char line[9000];

    make_dir(dir, sizeof dir);
    assert(db_open(dir, &db, &err) == DB_OK);

    assert(db_get_table(&db, "userz", &t, &err) == DB_ERR_NO_TABLE);
    assert(strcmp(err.message, "Error: table 'userz' does not exist") == 0);
    assert(db.table_count == 0);

    write_file(dir, "bad.schema", "name,TEXT\n");
    assert(db_get_table(&db, "bad", &t, &err) == DB_ERR_SCHEMA);

    memset(name, 'n', 64);
    name[64] = '\0';
    assert(db_get_table(&db, name, &t, &err) == DB_ERR_TOO_LONG);

    // 테이블 8개까지는 열리고 9번째는 안 된다
    for (int i = 0; i < 9; i++) {
        snprintf(name, sizeof name, "t%d.schema", i);
        write_file(dir, name, USERS_SCHEMA);
    }
    for (int i = 0; i < 8; i++) {
        snprintf(name, sizeof name, "t%d", i);
        assert(db_get_table(&db, name, &t, &err) == DB_OK);
    }
    assert(db_get_table(&db, "t8", &t, &err) == DB_ERR_TOO_LONG);
    assert(db_get_table(&db, "t0", &t, &err) == DB_OK);     // 이미 연 것은 계속 쓸 수 있다

    // 줄바꿈까지 8192바이트는 되고 8193바이트는 안 된다
    memset(line, 'a', 8192);
    line[8191] = '\0';
    assert(storage_append(t, line, &offset, &err) == DB_OK);
    line[8191] = 'a';
    line[8192] = '\0';
    assert(storage_append(t, line, &offset, &err) == DB_ERR_TOO_LONG);

    db_close(&db);
    assert(db.table_count == 0);
    remove_dir(dir);

    memset(long_dir, 'd', 300);
    long_dir[300] = '\0';
    assert(db_open(long_dir, &db, &err) == DB_ERR_TOO_LONG);
}

int main(void)
{
    test_schema_load();
    test_schema_errors();
    test_append_and_reopen();
    test_scan_values();
    test_corrupt_data();
    test_db_errors();
    printf("test_storage: ok\n");
    return 0;
}
