#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "statement.h"
#include "token.h"

// SQL 문장 하나를 토큰으로 만든 뒤 파싱한다. 토크나이저는 통과하는 입력만 넣는다.
static DbStatus parse_sql(const char *sql, Statement *stmt, DbError *err)
{
    Token toks[MAX_TOKENS];
    int pos = 0, count = 0;

    assert(tokenize_next(sql, &pos, toks, MAX_TOKENS, &count, err) == DB_OK);
    return parse(toks, count, stmt, err);
}

static void test_insert(void)
{
    Statement s;
    DbError err;

    assert(parse_sql("INSERT INTO users VALUES ('Kim, Jr', 20);", &s, &err) == DB_OK);
    assert(s.type == STMT_INSERT);
    assert(strcmp(s.table, "users") == 0);
    assert(s.value_count == 2);
    assert(s.values[0].type == VAL_STR && strcmp(s.values[0].s, "Kim, Jr") == 0);
    assert(s.values[1].type == VAL_INT && s.values[1].i == 20);

    // 소문자 키워드, 음수, 빈 문자열, 끝의 ; 생략
    assert(parse_sql("insert into t values (-5, '')", &s, &err) == DB_OK);
    assert(s.value_count == 2);
    assert(s.values[0].type == VAL_INT && s.values[0].i == -5);
    assert(s.values[1].type == VAL_STR && strcmp(s.values[1].s, "") == 0);
}

static void test_select_all(void)
{
    Statement s;
    DbError err;

    assert(parse_sql("SELECT * FROM users;", &s, &err) == DB_OK);
    assert(s.type == STMT_SELECT);
    assert(strcmp(s.table, "users") == 0);
    assert(s.select_all == 1 && s.column_count == 0);
    assert(s.where == NULL);
}

static void test_select_columns_and_where(void)
{
    Statement s;
    DbError err;

    assert(parse_sql("SELECT name, id FROM users WHERE id = 1;", &s, &err) == DB_OK);
    assert(s.select_all == 0 && s.column_count == 2);
    assert(strcmp(s.columns[0], "name") == 0);
    assert(strcmp(s.columns[1], "id") == 0);
    assert(s.where != NULL && s.where->type == COND_COMPARE);
    assert(strcmp(s.where->column, "id") == 0);
    assert(s.where->op == OP_EQ);
    assert(s.where->value.type == VAL_INT && s.where->value.i == 1);
    statement_free(&s);
    assert(s.where == NULL);

    assert(parse_sql("select * from users where name = 'Kim'", &s, &err) == DB_OK);
    assert(s.where != NULL);
    assert(s.where->value.type == VAL_STR && strcmp(s.where->value.s, "Kim") == 0);
    statement_free(&s);
}

// 앞 문장의 결과가 다음 문장에 남으면 안 된다
static void test_statement_is_reset(void)
{
    Statement s;
    DbError err;

    assert(parse_sql("SELECT * FROM users WHERE id = 1", &s, &err) == DB_OK);
    statement_free(&s);
    assert(parse_sql("SELECT name FROM users", &s, &err) == DB_OK);
    assert(s.select_all == 0 && s.where == NULL && s.column_count == 1);
}

static void test_syntax_errors(void)
{
    Statement s;
    DbError err;

    assert(parse_sql("SELECT FROM users", &s, &err) == DB_ERR_SYNTAX);
    assert(err.code == DB_ERR_SYNTAX && err.pos == 7);
    assert(strcmp(err.message, "Syntax error at position 7: "
                               "expected '*' or column name, but got 'FROM'") == 0);

    assert(parse_sql("SELECT * FROM users abc", &s, &err) == DB_ERR_SYNTAX);
    assert(err.pos == 20);
    assert(strcmp(err.message, "Syntax error at position 20: "
                               "expected end of statement, but got 'abc'") == 0);

    // 문장이 중간에 끊긴 경우: 위치는 입력의 끝이다
    assert(parse_sql("INSERT INTO users VALUES ('Kim'", &s, &err) == DB_ERR_SYNTAX);
    assert(err.pos == 31);
    assert(strcmp(err.message, "Syntax error at position 31: "
                               "expected ',' or ')', but got end of input") == 0);

    assert(parse_sql("INSERT INTO users VALUES ()", &s, &err) == DB_ERR_SYNTAX);
    assert(parse_sql("INSERT INTO users VALUES (1,)", &s, &err) == DB_ERR_SYNTAX);
    assert(parse_sql("INSERT INTO users VALUES (name)", &s, &err) == DB_ERR_SYNTAX);
    assert(parse_sql("INSERT users VALUES (1)", &s, &err) == DB_ERR_SYNTAX);
    assert(parse_sql("SELECT *, name FROM users", &s, &err) == DB_ERR_SYNTAX);
    assert(parse_sql("SELECT name, FROM users", &s, &err) == DB_ERR_SYNTAX);
    assert(parse_sql("SELECT * FROM users WHERE id", &s, &err) == DB_ERR_SYNTAX);
    assert(parse_sql("SELECT * FROM users WHERE id = ", &s, &err) == DB_ERR_SYNTAX);
    assert(parse_sql("SELECT * FROM users WHERE id = 1 extra", &s, &err) == DB_ERR_SYNTAX);
    assert(parse_sql("SELECT * FROM users;;", &s, &err) == DB_OK);  // 둘째 ; 는 다음 문장이다

    // 범위 밖 문장과 빈 문장
    assert(parse_sql("UPDATE users", &s, &err) == DB_ERR_SYNTAX);
    assert(err.pos == 0);
    assert(parse_sql(";", &s, &err) == DB_ERR_SYNTAX);
    assert(parse_sql("", &s, &err) == DB_ERR_SYNTAX);
    assert(parse(NULL, 0, &s, &err) == DB_ERR_SYNTAX);
}

// "INSERT INTO t VALUES (1, 1, ...)" 를 값 n 개로 만든다
static void build_insert(char *sql, int n)
{
    strcpy(sql, "INSERT INTO t VALUES (");
    for (int i = 0; i < n; i++) {
        strcat(sql, i == 0 ? "1" : ", 1");
    }
    strcat(sql, ")");
}

// "SELECT c, c, ... FROM t" 를 컬럼 n 개로 만든다
static void build_select(char *sql, int n)
{
    strcpy(sql, "SELECT ");
    for (int i = 0; i < n; i++) {
        strcat(sql, i == 0 ? "c" : ", c");
    }
    strcat(sql, " FROM t");
}

static void test_limits(void)
{
    Statement s;
    DbError err;
    char sql[256];

    build_insert(sql, 16);
    assert(parse_sql(sql, &s, &err) == DB_OK);
    assert(s.value_count == 16);
    build_insert(sql, 17);
    assert(parse_sql(sql, &s, &err) == DB_ERR_TOO_LONG);
    assert(err.code == DB_ERR_TOO_LONG);

    build_select(sql, 16);
    assert(parse_sql(sql, &s, &err) == DB_OK);
    assert(s.column_count == 16);
    build_select(sql, 17);
    assert(parse_sql(sql, &s, &err) == DB_ERR_TOO_LONG);
}

// 토크나이저를 거치지 않은 토큰이 와도 이름 버퍼(64)를 넘겨 쓰지 않는다
static void test_long_name_token(void)
{
    Token toks[3] = {
        { .type = TOK_SELECT, .text = "SELECT", .pos = 0 },
        { .type = TOK_IDENTIFIER, .pos = 7 },
        { .type = TOK_EOF, .pos = 107 },
    };
    Statement s;
    DbError err;

    memset(toks[1].text, 'n', 100);
    assert(parse(toks, 3, &s, &err) == DB_ERR_TOO_LONG);
}

// 끝에 EOF 가 없는 토큰 배열이 와도 배열 밖을 읽지 않는다
static void test_missing_eof(void)
{
    Token toks[MAX_TOKENS];
    Statement s;
    DbError err;
    int pos = 0, count = 0;

    assert(tokenize_next("INSERT INTO t VALUES (1, 2", &pos, toks, MAX_TOKENS, &count, &err) == DB_OK);
    assert(parse(toks, count - 1, &s, &err) == DB_ERR_SYNTAX);
    assert(parse(toks, 6, &s, &err) == DB_ERR_SYNTAX);  // "... 1 ," 에서 끊김
}

// 조건 하나짜리 잎인지, 컬럼 이름이 맞는지 본다
static int is_leaf(const Cond *c, const char *column)
{
    return c != NULL && c->type == COND_COMPARE && strcmp(c->column, column) == 0
        && c->left == NULL && c->right == NULL;
}

static void test_where_tree(void)
{
    Statement s;
    DbError err;

    // AND 가 OR 보다 먼저 묶인다: a OR (b AND c)
    assert(parse_sql("SELECT * FROM t WHERE a = 1 OR b = 2 AND c = 3", &s, &err) == DB_OK);
    assert(s.where->type == COND_OR);
    assert(is_leaf(s.where->left, "a"));
    assert(s.where->right->type == COND_AND);
    assert(is_leaf(s.where->right->left, "b") && is_leaf(s.where->right->right, "c"));
    statement_free(&s);

    // 괄호로 순서를 바꾼다: (a OR b) AND c
    assert(parse_sql("SELECT * FROM t WHERE (a = 1 OR b = 2) AND c = 3", &s, &err) == DB_OK);
    assert(s.where->type == COND_AND);
    assert(s.where->left->type == COND_OR);
    assert(is_leaf(s.where->left->left, "a") && is_leaf(s.where->left->right, "b"));
    assert(is_leaf(s.where->right, "c"));
    statement_free(&s);

    // 같은 연산자는 왼쪽부터 묶인다: (a AND b) AND c
    assert(parse_sql("select * from t where a = 1 and b = 2 and c = 3", &s, &err) == DB_OK);
    assert(s.where->type == COND_AND && s.where->left->type == COND_AND);
    assert(is_leaf(s.where->left->left, "a") && is_leaf(s.where->right, "c"));
    statement_free(&s);

    // NOT 은 AND 보다 먼저 묶인다: (NOT a) AND b
    assert(parse_sql("SELECT * FROM t WHERE NOT a = 1 AND b = 2", &s, &err) == DB_OK);
    assert(s.where->type == COND_AND);
    assert(s.where->left->type == COND_NOT && s.where->left->right == NULL);
    assert(is_leaf(s.where->left->left, "a") && is_leaf(s.where->right, "b"));
    statement_free(&s);

    assert(parse_sql("SELECT * FROM t WHERE NOT NOT ((a = 1))", &s, &err) == DB_OK);
    assert(s.where->type == COND_NOT && s.where->left->type == COND_NOT);
    assert(is_leaf(s.where->left->left, "a"));
    statement_free(&s);
}

static void test_where_operators(void)
{
    const struct { const char *text; CompareOp op; } cases[] = {
        {"=", OP_EQ}, {"!=", OP_NE}, {"<>", OP_NE}, {"<", OP_LT},
        {">", OP_GT}, {"<=", OP_LE}, {">=", OP_GE},
    };
    Statement s;
    DbError err;
    char sql[64];

    for (int i = 0; i < 7; i++) {
        snprintf(sql, sizeof sql, "SELECT * FROM t WHERE a %s -5", cases[i].text);
        assert(parse_sql(sql, &s, &err) == DB_OK);
        assert(is_leaf(s.where, "a") && s.where->op == cases[i].op);
        assert(s.where->value.type == VAL_INT && s.where->value.i == -5);
        statement_free(&s);
    }
}

// 조건이 틀리면 문법 오류이고, 만들던 트리는 남지 않는다 (남으면 make asan 이 누수로 잡는다)
static void test_where_errors(void)
{
    const char *bad[] = {
        "SELECT * FROM t WHERE",
        "SELECT * FROM t WHERE a = 1 AND",
        "SELECT * FROM t WHERE a = 1 OR OR b = 2",
        "SELECT * FROM t WHERE AND a = 1",
        "SELECT * FROM t WHERE NOT",
        "SELECT * FROM t WHERE a 1",
        "SELECT * FROM t WHERE a = b",
        "SELECT * FROM t WHERE ()",
        "SELECT * FROM t WHERE a = 1)",
        "SELECT * FROM t WHERE (a = 1 OR b = 2 c = 3)",
        "SELECT * FROM t WHERE a = 1 AND (b = 2 OR (c = 3 AND d = 4) e",
        "SELECT * FROM t WHERE a = 1 AND b = 2 abc",
    };
    Statement s;
    DbError err;

    for (int i = 0; i < (int)(sizeof bad / sizeof bad[0]); i++) {
        assert(parse_sql(bad[i], &s, &err) == DB_ERR_SYNTAX);
        assert(s.where == NULL);
    }

    assert(parse_sql("SELECT * FROM t WHERE (a = 1", &s, &err) == DB_ERR_SYNTAX);
    assert(strcmp(err.message, "Syntax error at position 28: "
                               "expected AND, OR or ')', but got end of input") == 0);
    assert(parse_sql("SELECT * FROM t WHERE a 1", &s, &err) == DB_ERR_SYNTAX);
    assert(strcmp(err.message, "Syntax error at position 24: "
                               "expected comparison operator, but got '1'") == 0);

    // 괄호를 여러 겹 쳐도 된다 (토큰 한도 안에서)
    assert(parse_sql("SELECT * FROM t WHERE ((((((((((a = 1))))))))))", &s, &err) == DB_OK);
    assert(is_leaf(s.where, "a"));
    statement_free(&s);
    statement_free(&s);     // 두 번 불러도 안전하다
}

int main(void)
{
    test_insert();
    test_select_all();
    test_select_columns_and_where();
    test_statement_is_reset();
    test_syntax_errors();
    test_where_tree();
    test_where_operators();
    test_where_errors();
    test_limits();
    test_long_name_token();
    test_missing_eof();
    printf("test_parser: ok\n");
    return 0;
}
