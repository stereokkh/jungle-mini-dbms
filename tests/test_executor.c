#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "db.h"
#include "executor.h"
#include "statement.h"
#include "token.h"

#define USERS_SCHEMA "id,INT\nname,TEXT\nage,INT\n"

// 테스트마다 새 임시 디렉터리에 users 테이블을 만들어 쓴다.
static void make_db(char *dir, size_t size, Db *db)
{
    char path[300];
    DbError err;

    snprintf(dir, size, "/tmp/mini-dbms-test-XXXXXX");
    assert(mkdtemp(dir) != NULL);
    snprintf(path, sizeof path, "%s/users.schema", dir);
    FILE *fp = fopen(path, "wb");
    assert(fp != NULL);
    fputs(USERS_SCHEMA, fp);
    fclose(fp);
    assert(db_open(dir, db, &err) == DB_OK);
}

static void remove_db(const char *dir, Db *db)
{
    char cmd[300];

    db_close(db);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    assert(system(cmd) == 0);
}

// 찾은 행을 모아 두는 콜백
typedef struct {
    Value rows[8][3];
    int   field_counts[8];
    int   count;
} Rows;

static void collect(const Value *fields, int field_count, void *ctx)
{
    Rows *r = ctx;

    assert(r->count < 8 && field_count <= 3);
    memcpy(r->rows[r->count], fields, sizeof(Value) * (size_t)field_count);
    r->field_counts[r->count] = field_count;
    r->count++;
}

// SQL 문장 하나를 토큰 → 파싱 → 실행까지 한다. 앞 두 단계는 통과하는 입력만 넣는다.
static DbStatus run(Db *db, const char *sql, Rows *rows, ExecResult *result, DbError *err)
{
    Token toks[MAX_TOKENS];
    Statement stmt;
    int pos = 0, count = 0;

    if (rows != NULL) {
        rows->count = 0;
    }
    assert(tokenize_next(sql, &pos, toks, MAX_TOKENS, &count, err) == DB_OK);
    assert(parse(toks, count, &stmt, err) == DB_OK);
    DbStatus st = execute(db, &stmt, collect, rows, result, err);
    statement_free(&stmt);
    return st;
}

static void insert_ok(Db *db, const char *sql, long long expected_id)
{
    ExecResult res;
    DbError err;

    assert(run(db, sql, NULL, &res, &err) == DB_OK);
    assert(res.inserted_id == expected_id);
}

static void test_insert_and_select_all(void)
{
    char dir[64];
    Db db;
    Rows rows;
    ExecResult res;
    DbError err;

    make_db(dir, sizeof dir, &db);

    // 데이터 파일이 없을 때 SELECT 는 0행이다
    assert(run(&db, "SELECT * FROM users", &rows, &res, &err) == DB_OK);
    assert(res.row_count == 0 && rows.count == 0);

    insert_ok(&db, "INSERT INTO users VALUES ('Kim, Jr', 20);", 1);
    insert_ok(&db, "INSERT INTO users VALUES ('Lee', 31);", 2);

    assert(run(&db, "select * from users", &rows, &res, &err) == DB_OK);
    assert(res.row_count == 2 && rows.count == 2);
    assert(rows.field_counts[0] == 3);
    assert(rows.rows[0][0].type == VAL_INT && rows.rows[0][0].i == 1);
    assert(rows.rows[0][1].type == VAL_STR && strcmp(rows.rows[0][1].s, "Kim, Jr") == 0);
    assert(rows.rows[0][2].type == VAL_INT && rows.rows[0][2].i == 20);
    assert(rows.rows[1][0].i == 2 && strcmp(rows.rows[1][1].s, "Lee") == 0);

    remove_db(dir, &db);
}

static void test_where_and_columns(void)
{
    char dir[64];
    Db db;
    Rows rows;
    ExecResult res;
    DbError err;

    make_db(dir, sizeof dir, &db);
    insert_ok(&db, "INSERT INTO users VALUES ('Kim', 20)", 1);
    insert_ok(&db, "INSERT INTO users VALUES ('Lee', 31)", 2);
    insert_ok(&db, "INSERT INTO users VALUES ('Kim', -5)", 3);

    assert(run(&db, "SELECT * FROM users WHERE id = 2", &rows, &res, &err) == DB_OK);
    assert(res.row_count == 1 && strcmp(rows.rows[0][1].s, "Lee") == 0);

    assert(run(&db, "SELECT * FROM users WHERE name = 'Kim'", &rows, &res, &err) == DB_OK);
    assert(res.row_count == 2 && rows.rows[0][0].i == 1 && rows.rows[1][0].i == 3);

    assert(run(&db, "SELECT * FROM users WHERE age = -5", &rows, &res, &err) == DB_OK);
    assert(res.row_count == 1 && rows.rows[0][0].i == 3);

    // 문자열 비교는 대소문자를 구분한다
    assert(run(&db, "SELECT * FROM users WHERE name = 'kim'", &rows, &res, &err) == DB_OK);
    assert(res.row_count == 0);

    // 고른 컬럼만, 고른 순서로. WHERE 컬럼은 고르지 않아도 된다
    assert(run(&db, "SELECT age, name FROM users WHERE id = 2", &rows, &res, &err) == DB_OK);
    assert(res.row_count == 1 && rows.field_counts[0] == 2);
    assert(rows.rows[0][0].type == VAL_INT && rows.rows[0][0].i == 31);
    assert(rows.rows[0][1].type == VAL_STR && strcmp(rows.rows[0][1].s, "Lee") == 0);

    remove_db(dir, &db);
}

// 넣은 문자열이 그대로 돌아오는지: 작은따옴표, 큰따옴표, 빈 문자열, 한글, 쉼표
static void test_text_round_trip(void)
{
    char dir[64];
    Db db;
    Rows rows;
    ExecResult res;
    DbError err;

    make_db(dir, sizeof dir, &db);
    insert_ok(&db, "INSERT INTO users VALUES ('O''Brien', 1)", 1);
    insert_ok(&db, "INSERT INTO users VALUES ('He said \"hi\"', 2)", 2);
    insert_ok(&db, "INSERT INTO users VALUES ('', 3)", 3);
    insert_ok(&db, "INSERT INTO users VALUES ('김건호', 4)", 4);

    assert(run(&db, "SELECT name FROM users", &rows, &res, &err) == DB_OK);
    assert(res.row_count == 4);
    assert(strcmp(rows.rows[0][0].s, "O'Brien") == 0);
    assert(strcmp(rows.rows[1][0].s, "He said \"hi\"") == 0);
    assert(strcmp(rows.rows[2][0].s, "") == 0);
    assert(strcmp(rows.rows[3][0].s, "김건호") == 0);

    assert(run(&db, "SELECT id FROM users WHERE name = 'He said \"hi\"'", &rows, &res, &err) == DB_OK);
    assert(res.row_count == 1 && rows.rows[0][0].i == 2);

    remove_db(dir, &db);
}

static void test_insert_errors(void)
{
    char dir[64];
    Db db;
    Rows rows;
    ExecResult res;
    DbError err;

    make_db(dir, sizeof dir, &db);

    assert(run(&db, "INSERT INTO userz VALUES ('Kim', 20)", NULL, &res, &err) == DB_ERR_NO_TABLE);
    assert(strcmp(err.message, "Error: table 'userz' does not exist") == 0);

    // id 까지 넣은 경우
    assert(run(&db, "INSERT INTO users VALUES (1, 'Kim', 20)", NULL, &res, &err) == DB_ERR_COLUMN_COUNT);
    assert(strcmp(err.message, "Error: expected 2 values (name, age), but got 3 "
                               "(id is assigned automatically)") == 0);

    // 값이 하나 부족한 경우
    assert(run(&db, "INSERT INTO users VALUES ('Kim')", NULL, &res, &err) == DB_ERR_COLUMN_COUNT);
    assert(strcmp(err.message, "Error: expected 2 values (name, age), but got 1") == 0);

    assert(run(&db, "INSERT INTO users VALUES ('Kim', 'twenty')", NULL, &res, &err) == DB_ERR_TYPE);
    assert(strcmp(err.message, "Error: column 'age' expects INT, but got TEXT") == 0);
    assert(run(&db, "INSERT INTO users VALUES (7, 20)", NULL, &res, &err) == DB_ERR_TYPE);
    assert(strcmp(err.message, "Error: column 'name' expects TEXT, but got INT") == 0);

    assert(run(&db, "INSERT INTO users VALUES ('two\nlines', 20)", NULL, &res, &err) == DB_ERR_TYPE);

    // 실패한 INSERT 는 아무것도 저장하지 않고 id 도 쓰지 않는다
    assert(run(&db, "SELECT * FROM users", &rows, &res, &err) == DB_OK);
    assert(res.row_count == 0);
    insert_ok(&db, "INSERT INTO users VALUES ('Kim', 20)", 1);

    remove_db(dir, &db);
}

static void test_select_errors(void)
{
    char dir[64];
    Db db;
    Rows rows;
    ExecResult res;
    DbError err;

    make_db(dir, sizeof dir, &db);
    insert_ok(&db, "INSERT INTO users VALUES ('Kim', 20)", 1);

    assert(run(&db, "SELECT * FROM userz", &rows, &res, &err) == DB_ERR_NO_TABLE);

    assert(run(&db, "SELECT * FROM users WHERE nope = 1", &rows, &res, &err) == DB_ERR_NO_COLUMN);
    assert(strcmp(err.message, "Error: column 'nope' does not exist in table 'users'") == 0);
    assert(run(&db, "SELECT name, nope FROM users", &rows, &res, &err) == DB_ERR_NO_COLUMN);
    assert(run(&db, "SELECT Name FROM users", &rows, &res, &err) == DB_ERR_NO_COLUMN);

    assert(run(&db, "SELECT * FROM users WHERE id = 'abc'", &rows, &res, &err) == DB_ERR_TYPE);
    assert(strcmp(err.message, "Error: column 'id' expects INT, but got TEXT") == 0);
    assert(run(&db, "SELECT * FROM users WHERE name = 1", &rows, &res, &err) == DB_ERR_TYPE);
    assert(rows.count == 0);    // 검사에서 걸리면 콜백은 한 번도 불리지 않는다

    remove_db(dir, &db);
}

// DB 를 닫고 다시 열어도 id 가 이어진다
static void test_reopen_continues_id(void)
{
    char dir[64];
    Db db;
    Rows rows;
    ExecResult res;
    DbError err;

    make_db(dir, sizeof dir, &db);
    insert_ok(&db, "INSERT INTO users VALUES ('Kim', 20)", 1);
    insert_ok(&db, "INSERT INTO users VALUES ('Lee', 31)", 2);
    db_close(&db);

    assert(db_open(dir, &db, &err) == DB_OK);
    insert_ok(&db, "INSERT INTO users VALUES ('Park', 40)", 3);
    assert(run(&db, "SELECT * FROM users", &rows, &res, &err) == DB_OK);
    assert(res.row_count == 3 && rows.rows[2][0].i == 3);

    remove_db(dir, &db);
}

// 만들 수 있는 가장 긴 행: TEXT 컬럼 15개에 큰따옴표 255개씩.
// " 는 "" 로 늘어나서 컬럼 하나가 512바이트, 한 줄이 약 7700바이트가 된다.
// 한 줄 한도(8192) 안이라 저장되고 다시 읽혀야 한다.
static void test_widest_row(void)
{
    char dir[64], path[300], sql[5000], text[256];
    Db db;
    Rows rows;
    ExecResult res;
    DbError err;

    make_db(dir, sizeof dir, &db);
    snprintf(path, sizeof path, "%s/wide.schema", dir);
    FILE *fp = fopen(path, "wb");
    assert(fp != NULL);
    fputs("id,INT\n", fp);
    for (int i = 0; i < 15; i++) {
        fprintf(fp, "c%d,TEXT\n", i);
    }
    fclose(fp);

    memset(text, '"', 255);
    text[255] = '\0';
    strcpy(sql, "INSERT INTO wide VALUES (");
    for (int i = 0; i < 15; i++) {
        strcat(sql, i == 0 ? "'" : ", '");
        strcat(sql, text);
        strcat(sql, "'");
    }
    strcat(sql, ")");
    assert(run(&db, sql, NULL, &res, &err) == DB_OK);
    assert(res.inserted_id == 1);

    assert(run(&db, "SELECT c14 FROM wide", &rows, &res, &err) == DB_OK);
    assert(res.row_count == 1 && strcmp(rows.rows[0][0].s, text) == 0);

    remove_db(dir, &db);
}

// 조건에 맞는 행의 id 를 이어 붙여 돌려준다. 예: "1,4"
static const char *ids_where(Db *db, const char *where, char *out, size_t size)
{
    char sql[256];
    Rows rows;
    ExecResult res;
    DbError err;
    size_t len = 0;

    snprintf(sql, sizeof sql, "SELECT id FROM users WHERE %s", where);
    assert(run(db, sql, &rows, &res, &err) == DB_OK);
    assert(res.row_count == rows.count);
    out[0] = '\0';
    for (int i = 0; i < rows.count; i++) {
        len += (size_t)snprintf(out + len, size - len, "%s%lld", i > 0 ? "," : "", rows.rows[i][0].i);
    }
    return out;
}

static void test_where_tree(void)
{
    char dir[64], ids[64];
    Db db;
    Rows rows;
    ExecResult res;
    DbError err;

    make_db(dir, sizeof dir, &db);
    insert_ok(&db, "INSERT INTO users VALUES ('Kim', 20)", 1);
    insert_ok(&db, "INSERT INTO users VALUES ('Lee', 31)", 2);
    insert_ok(&db, "INSERT INTO users VALUES ('Park', 40)", 3);
    insert_ok(&db, "INSERT INTO users VALUES ('Choi', 31)", 4);
#define IDS(where) ids_where(&db, where, ids, sizeof ids)

    // 비교 연산자
    assert(strcmp(IDS("age = 31"), "2,4") == 0);
    assert(strcmp(IDS("age != 31"), "1,3") == 0);
    assert(strcmp(IDS("age <> 31"), "1,3") == 0);
    assert(strcmp(IDS("age < 31"), "1") == 0);
    assert(strcmp(IDS("age > 31"), "3") == 0);
    assert(strcmp(IDS("age <= 31"), "1,2,4") == 0);
    assert(strcmp(IDS("age >= 31"), "2,3,4") == 0);
    assert(strcmp(IDS("age > 100"), "") == 0);

    // TEXT 의 크기 비교는 글자(바이트) 순서다
    assert(strcmp(IDS("name > 'Kim'"), "2,3") == 0);
    assert(strcmp(IDS("name <= 'Kim'"), "1,4") == 0);
    assert(strcmp(IDS("name != 'Kim'"), "2,3,4") == 0);

    // AND, OR, NOT, 괄호
    assert(strcmp(IDS("age = 31 AND name = 'Lee'"), "2") == 0);
    assert(strcmp(IDS("name = 'Lee' OR name = 'Park'"), "2,3") == 0);
    assert(strcmp(IDS("NOT age = 31"), "1,3") == 0);
    assert(strcmp(IDS("NOT (name = 'Kim' OR name = 'Lee')"), "3,4") == 0);
    assert(strcmp(IDS("id = 1 OR age = 31 AND name = 'Choi'"), "1,4") == 0);     // AND 먼저
    assert(strcmp(IDS("(id = 1 OR age = 31) AND name = 'Choi'"), "4") == 0);
    assert(strcmp(IDS("age >= 20 AND age <= 31 AND NOT id = 2"), "1,4") == 0);
    assert(strcmp(IDS("id = 1 OR id = 2 OR id = 3 OR id = 4"), "1,2,3,4") == 0);
#undef IDS

    // 트리 어디에 있든 없는 컬럼과 타입 불일치를 잡는다. 컬럼 검사가 타입 검사보다 먼저다
    assert(run(&db, "SELECT * FROM users WHERE id = 1 AND (age = 2 OR nope = 3)",
               &rows, &res, &err) == DB_ERR_NO_COLUMN);
    assert(strcmp(err.message, "Error: column 'nope' does not exist in table 'users'") == 0);
    assert(run(&db, "SELECT * FROM users WHERE id = 'x' OR nope = 1",
               &rows, &res, &err) == DB_ERR_NO_COLUMN);
    assert(run(&db, "SELECT * FROM users WHERE id = 1 OR NOT name = 5",
               &rows, &res, &err) == DB_ERR_TYPE);
    assert(strcmp(err.message, "Error: column 'name' expects TEXT, but got INT") == 0);
    assert(run(&db, "SELECT * FROM users WHERE age < 'a'", &rows, &res, &err) == DB_ERR_TYPE);
    assert(rows.count == 0);

    remove_db(dir, &db);
}

// `WHERE id = N` 은 인덱스로 찾는다. 리프가 나뉠 만큼(128개 초과) 넣고 확인한다.
// 결과는 선형 탐색과 같아야 하므로, 같은 행을 선형 탐색 조건(id >= N AND id <= N)으로도 찾아 비교한다.
static void test_id_index(void)
{
    char dir[64], sql[128], a[64], b[64];
    Db db;
    Rows rows;
    ExecResult res;
    DbError err;
    Table *t;

    make_db(dir, sizeof dir, &db);
    for (int i = 1; i <= 300; i++) {
        snprintf(sql, sizeof sql, "INSERT INTO users VALUES ('user%d', %d)", i, i % 50);
        insert_ok(&db, sql, i);
    }
    assert(db_get_table(&db, "users", &t, &err) == DB_OK);
    assert(t->index->count == 300 && t->index->height == 2);

    // 처음, 가운데, 끝
    assert(run(&db, "SELECT * FROM users WHERE id = 1", &rows, &res, &err) == DB_OK);
    assert(res.row_count == 1 && strcmp(rows.rows[0][1].s, "user1") == 0);
    assert(run(&db, "SELECT name, age FROM users WHERE id = 177", &rows, &res, &err) == DB_OK);
    assert(res.row_count == 1 && rows.field_counts[0] == 2);
    assert(strcmp(rows.rows[0][0].s, "user177") == 0 && rows.rows[0][1].i == 27);
    assert(run(&db, "SELECT * FROM users WHERE id = 300", &rows, &res, &err) == DB_OK);
    assert(res.row_count == 1 && strcmp(rows.rows[0][1].s, "user300") == 0);

    // 없는 id 는 0행
    assert(run(&db, "SELECT * FROM users WHERE id = 301", &rows, &res, &err) == DB_OK);
    assert(res.row_count == 0);
    assert(run(&db, "SELECT * FROM users WHERE id = -1", &rows, &res, &err) == DB_OK);
    assert(res.row_count == 0);

    // 인덱스 탐색과 선형 탐색의 결과가 같다
    assert(strcmp(ids_where(&db, "id = 250", a, sizeof a),
                  ids_where(&db, "id >= 250 AND id <= 250", b, sizeof b)) == 0);
    assert(strcmp(a, "250") == 0);

    // 다시 열면 파일을 훑어 인덱스를 다시 만든다
    db_close(&db);
    assert(db_open(dir, &db, &err) == DB_OK);
    assert(run(&db, "SELECT * FROM users WHERE id = 299", &rows, &res, &err) == DB_OK);
    assert(res.row_count == 1 && strcmp(rows.rows[0][1].s, "user299") == 0);
    assert(db_get_table(&db, "users", &t, &err) == DB_OK);
    assert(t->index->count == 300);
    insert_ok(&db, "INSERT INTO users VALUES ('late', 1)", 301);
    assert(run(&db, "SELECT * FROM users WHERE id = 301", &rows, &res, &err) == DB_OK);
    assert(res.row_count == 1 && strcmp(rows.rows[0][1].s, "late") == 0);

    remove_db(dir, &db);
}

int main(void)
{
    test_id_index();
    test_insert_and_select_all();
    test_where_and_columns();
    test_text_round_trip();
    test_insert_errors();
    test_select_errors();
    test_where_tree();
    test_reopen_continues_id();
    test_widest_row();
    printf("test_executor: ok\n");
    return 0;
}
