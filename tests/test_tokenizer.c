#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "token.h"

// 문장 하나만 들어 있는 입력을 토큰으로 만든다. 대부분의 테스트가 이 모양이라 묶었다.
static DbStatus tokenize_one(const char *sql, Token *toks, int *count, DbError *err)
{
    int pos = 0;
    return tokenize_next(sql, &pos, toks, MAX_TOKENS, count, err);
}

static void test_insert_statement(void)
{
    Token t[MAX_TOKENS];
    DbError err;
    int count = 0;
    TokenType expected[] = {
        TOK_INSERT, TOK_INTO, TOK_IDENTIFIER, TOK_VALUES, TOK_LPAREN,
        TOK_STRING, TOK_COMMA, TOK_INTEGER, TOK_RPAREN, TOK_SEMICOLON, TOK_EOF
    };

    assert(tokenize_one("INSERT INTO users VALUES ('Kim, Jr', 20);", t, &count, &err) == DB_OK);
    assert(count == 11);    // 토큰 10개 + EOF
    for (int i = 0; i < count; i++) {
        assert(t[i].type == expected[i]);
    }
    assert(strcmp(t[2].text, "users") == 0);
    assert(strcmp(t[5].text, "Kim, Jr") == 0);
    assert(t[7].int_value == 20);
    assert(t[0].pos == 0);
    assert(t[2].pos == 12);
    assert(t[5].pos == 26);
}

static void test_two_statements(void)
{
    const char *sql = "SELECT * FROM a; SELECT * FROM b";
    Token t[MAX_TOKENS];
    DbError err;
    int pos = 0, count = 0;

    assert(tokenize_next(sql, &pos, t, MAX_TOKENS, &count, &err) == DB_OK);
    assert(count == 6);     // SELECT * FROM a ; EOF
    assert(strcmp(t[3].text, "a") == 0);
    assert(pos == 16);      // ; 바로 뒤

    // 마지막 문장은 ; 가 없어도 된다
    assert(tokenize_next(sql, &pos, t, MAX_TOKENS, &count, &err) == DB_OK);
    assert(count == 5);     // SELECT * FROM b EOF
    assert(strcmp(t[3].text, "b") == 0);
    assert(t[0].pos == 17); // 위치는 입력 전체 기준

    // 더 읽을 것이 없으면 EOF 하나만 나온다
    assert(tokenize_next(sql, &pos, t, MAX_TOKENS, &count, &err) == DB_OK);
    assert(count == 1 && t[0].type == TOK_EOF);
}

static void test_keywords_ignore_case(void)
{
    Token t[MAX_TOKENS];
    DbError err;
    int count = 0;

    assert(tokenize_one("select * from Users where Id = 1", t, &count, &err) == DB_OK);
    assert(t[0].type == TOK_SELECT);
    assert(t[1].type == TOK_STAR);
    assert(t[2].type == TOK_FROM);
    assert(t[4].type == TOK_WHERE);
    assert(t[6].type == TOK_EQ);
    // 이름은 대소문자를 그대로 둔다
    assert(t[3].type == TOK_IDENTIFIER && strcmp(t[3].text, "Users") == 0);
    assert(t[5].type == TOK_IDENTIFIER && strcmp(t[5].text, "Id") == 0);
}

static void test_strings(void)
{
    Token t[MAX_TOKENS];
    DbError err;
    int count = 0;

    assert(tokenize_one("'O''Brien'", t, &count, &err) == DB_OK);
    assert(t[0].type == TOK_STRING && strcmp(t[0].text, "O'Brien") == 0);

    assert(tokenize_one("''", t, &count, &err) == DB_OK);
    assert(t[0].type == TOK_STRING && strcmp(t[0].text, "") == 0);

    assert(tokenize_one("'He said \"hi\"'", t, &count, &err) == DB_OK);
    assert(strcmp(t[0].text, "He said \"hi\"") == 0);

    assert(tokenize_one("'김건호'", t, &count, &err) == DB_OK);
    assert(strcmp(t[0].text, "김건호") == 0);

    // 문자열 안의 ; 는 문장 끝이 아니다
    assert(tokenize_one("'a;b';", t, &count, &err) == DB_OK);
    assert(count == 3 && strcmp(t[0].text, "a;b") == 0);
}

static void test_unterminated_string(void)
{
    Token t[MAX_TOKENS];
    DbError err;
    int count = 0;

    assert(tokenize_one("SELECT 'Kim", t, &count, &err) == DB_ERR_SYNTAX);
    assert(err.code == DB_ERR_SYNTAX);
    assert(err.pos == 7);   // 여는 따옴표 위치
    assert(strcmp(err.message, "Syntax error at position 7: unterminated string") == 0);
}

static void test_integers(void)
{
    Token t[MAX_TOKENS];
    DbError err;
    int count = 0;

    assert(tokenize_one("-5", t, &count, &err) == DB_OK);
    assert(count == 2 && t[0].type == TOK_INTEGER && t[0].int_value == -5);

    assert(tokenize_one("age = -5", t, &count, &err) == DB_OK);
    assert(t[2].type == TOK_INTEGER && t[2].int_value == -5);

    assert(tokenize_one("-9223372036854775808", t, &count, &err) == DB_OK);
    assert(t[0].int_value == LLONG_MIN);

    assert(tokenize_one("99999999999999999999", t, &count, &err) == DB_ERR_SYNTAX);
    assert(err.pos == 0);

    // 숫자가 따라오지 않는 - 는 받아 주지 않는다
    assert(tokenize_one("- 5", t, &count, &err) == DB_ERR_SYNTAX);
}

static void test_limits(void)
{
    Token t[MAX_TOKENS];
    DbError err;
    int count = 0;
    char sql[1024];

    // 255바이트 문자열은 되고 300바이트는 안 된다
    sql[0] = '\'';
    memset(sql + 1, 'a', 255);
    strcpy(sql + 256, "'");
    assert(tokenize_one(sql, t, &count, &err) == DB_OK);
    assert(strlen(t[0].text) == 255);

    memset(sql + 1, 'a', 300);
    strcpy(sql + 301, "'");
    assert(tokenize_one(sql, t, &count, &err) == DB_ERR_TOO_LONG);

    // 63자 이름은 되고 64자는 안 된다
    memset(sql, 'n', 63);
    sql[63] = '\0';
    assert(tokenize_one(sql, t, &count, &err) == DB_OK);
    memset(sql, 'n', 64);
    sql[64] = '\0';
    assert(tokenize_one(sql, t, &count, &err) == DB_ERR_TOO_LONG);

    // 토큰이 64개(EOF 포함)를 넘으면 버퍼를 넘겨 쓰지 않고 오류를 돌려준다
    sql[0] = '\0';
    for (int i = 0; i < 63; i++) {
        strcat(sql, "a ");
    }
    assert(tokenize_one(sql, t, &count, &err) == DB_OK);
    assert(count == 64);
    strcat(sql, "a ");
    assert(tokenize_one(sql, t, &count, &err) == DB_ERR_TOO_LONG);
}

static void test_empty_inputs(void)
{
    Token t[MAX_TOKENS];
    DbError err;
    int count = 0;

    assert(tokenize_one("", t, &count, &err) == DB_OK);
    assert(count == 1 && t[0].type == TOK_EOF);

    assert(tokenize_one("  \n\t ", t, &count, &err) == DB_OK);
    assert(count == 1 && t[0].type == TOK_EOF);

    assert(tokenize_one(";", t, &count, &err) == DB_OK);
    assert(count == 2 && t[0].type == TOK_SEMICOLON && t[1].type == TOK_EOF);
}

static void test_unexpected_character(void)
{
    Token t[MAX_TOKENS];
    DbError err;
    int pos = 0, count = 0;

    assert(tokenize_next("SELECT @", &pos, t, MAX_TOKENS, &count, &err) == DB_ERR_SYNTAX);
    assert(err.pos == 7);
    assert(strcmp(err.message, "Syntax error at position 7: unexpected character '@'") == 0);
    assert(pos == 0);       // 실패하면 *pos 는 그대로다

    // 따옴표 밖의 한글은 이름으로 받지 않는다
    assert(tokenize_one("SELECT 이름", t, &count, &err) == DB_ERR_SYNTAX);
}

static void test_operators(void)
{
    Token t[MAX_TOKENS];
    DbError err;
    int count = 0;
    TokenType expected[] = {
        TOK_EQ, TOK_NE, TOK_NE, TOK_LT, TOK_GT, TOK_LE, TOK_GE,
        TOK_AND, TOK_OR, TOK_NOT, TOK_EOF
    };

    assert(tokenize_one("= != <> < > <= >= AND or Not", t, &count, &err) == DB_OK);
    assert(count == 11);
    for (int i = 0; i < count; i++) {
        assert(t[i].type == expected[i]);
    }
    assert(strcmp(t[5].text, "<=") == 0);

    // 붙여 써도 나뉜다. <= 뒤의 -5 는 음수 하나다
    assert(tokenize_one("a<=-5", t, &count, &err) == DB_OK);
    assert(count == 4 && t[1].type == TOK_LE && t[2].type == TOK_INTEGER && t[2].int_value == -5);
    assert(tokenize_one("a<b", t, &count, &err) == DB_OK);
    assert(count == 4 && t[1].type == TOK_LT);

    // ! 혼자는 기호가 아니다
    assert(tokenize_one("a ! 1", t, &count, &err) == DB_ERR_SYNTAX);
    assert(err.pos == 2);
}

int main(void)
{
    test_insert_statement();
    test_two_statements();
    test_keywords_ignore_case();
    test_strings();
    test_unterminated_string();
    test_integers();
    test_limits();
    test_empty_inputs();
    test_unexpected_character();
    test_operators();
    printf("test_tokenizer: ok\n");
    return 0;
}
