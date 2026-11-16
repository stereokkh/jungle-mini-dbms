#include "token.h"
#include "statement.h"   // 이름 한도를 Statement.table 의 크기에서 얻으려고 가져온다

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// ctype 함수에 음수(한글 바이트 등)를 넘기면 정의되지 않은 동작이라 unsigned char 로 바꿔서 넘긴다.
static int is_name_start(char c)
{
    return isalpha((unsigned char)c) || c == '_';
}

static int is_name_char(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

static int is_digit(char c)
{
    return isdigit((unsigned char)c);
}

// 키워드면 그 토큰 종류를, 아니면 TOK_IDENTIFIER 를 돌려준다. 키워드는 대소문자를 가리지 않는다.
static TokenType word_type(const char *word)
{
    const struct { const char *name; TokenType type; } keywords[] = {
        {"INSERT", TOK_INSERT}, {"INTO", TOK_INTO},     {"VALUES", TOK_VALUES},
        {"SELECT", TOK_SELECT}, {"FROM", TOK_FROM},     {"WHERE", TOK_WHERE},
        {"AND", TOK_AND},       {"OR", TOK_OR},         {"NOT", TOK_NOT},
    };
    int n = (int)(sizeof keywords / sizeof keywords[0]);

    for (int i = 0; i < n; i++) {
        if (strcasecmp(word, keywords[i].name) == 0) {
            return keywords[i].type;
        }
    }
    return TOK_IDENTIFIER;
}

// 키워드 또는 이름. text 에는 원문에 쓴 그대로 담는다 (이름은 대소문자를 구분하므로).
static DbStatus read_word(const char *sql, int *p, Token *t, DbError *err)
{
    int start = *p;
    int len = 0;
    // 한도를 숫자로 따로 적지 않고 이름이 실제로 담길 버퍼(char[64])에서 얻는다. 끝의 '\0' 한 칸을 뺀다.
    int max_len = (int)sizeof ((Statement *)0)->table - 1;

    while (is_name_char(sql[start + len])) {
        len++;
    }
    if (len > max_len) {
        return error_set(err, DB_ERR_TOO_LONG, start,
                         "name is longer than %d characters", max_len);
    }
    memcpy(t->text, sql + start, (size_t)len);
    t->text[len] = '\0';
    t->type = word_type(t->text);
    *p = start + len;
    return DB_OK;
}

// 정수. 부호(-)까지 한 토큰으로 읽는다. 호출하는 쪽이 "- 바로 뒤에 숫자"임을 확인하고 부른다.
static DbStatus read_integer(const char *sql, int *p, Token *t, DbError *err)
{
    int start = *p;
    int len = (sql[start] == '-') ? 1 : 0;
    int max_len = (int)sizeof t->text - 1;   // text 버퍼에서 '\0' 한 칸을 뺀 값
    char *end;

    while (is_digit(sql[start + len])) {
        len++;
    }
    if (len > max_len) {
        return error_set(err, DB_ERR_TOO_LONG, start,
                         "number is longer than %d characters", max_len);
    }
    // 직접 곱하고 더하면 넘침 검사를 손으로 써야 한다. strtoll 은 넘치면 errno 로 알려 준다.
    errno = 0;
    t->int_value = strtoll(sql + start, &end, 10);
    if (errno == ERANGE) {
        return error_set(err, DB_ERR_SYNTAX, start, "integer is out of range");
    }
    memcpy(t->text, sql + start, (size_t)len);
    t->text[len] = '\0';
    t->type = TOK_INTEGER;
    *p = start + len;
    return DB_OK;
}

// 문자열. 따옴표를 떼고 '' 를 ' 로 바꿔서 text 에 복사한다.
static DbStatus read_string(const char *sql, int *p, Token *t, DbError *err)
{
    int start = *p;
    int i = start + 1;      // 여는 따옴표 다음부터
    int len = 0;
    int max_len = (int)sizeof t->text - 1;   // text 버퍼에서 '\0' 한 칸을 뺀 값

    for (;;) {
        if (sql[i] == '\0') {
            return error_set(err, DB_ERR_SYNTAX, start, "unterminated string");
        }
        if (sql[i] == '\'') {
            if (sql[i + 1] != '\'') {
                break;      // 닫는 따옴표
            }
            i++;            // '' 의 첫 번째는 건너뛰고 두 번째를 글자로 담는다
        }
        if (len >= max_len) {
            return error_set(err, DB_ERR_TOO_LONG, start,
                             "string is longer than %d bytes", max_len);
        }
        t->text[len++] = sql[i++];
    }
    t->text[len] = '\0';
    t->type = TOK_STRING;
    *p = i + 1;             // 닫는 따옴표 다음
    return DB_OK;
}

// 기호 하나. 표의 순서대로 맞춰 보므로 두 글자짜리(<=)를 한 글자짜리(<)보다 앞에 둔다.
static DbStatus read_symbol(const char *sql, int *p, Token *t, DbError *err)
{
    const struct { const char *text; TokenType type; } symbols[] = {
        {"!=", TOK_NE},    {"<>", TOK_NE},    {"<=", TOK_LE},   {">=", TOK_GE},
        {"(", TOK_LPAREN}, {")", TOK_RPAREN}, {",", TOK_COMMA}, {";", TOK_SEMICOLON},
        {"*", TOK_STAR},   {"=", TOK_EQ},     {"<", TOK_LT},    {">", TOK_GT},
    };
    int n = (int)(sizeof symbols / sizeof symbols[0]);
    char c = sql[*p];

    for (int i = 0; i < n; i++) {
        size_t len = strlen(symbols[i].text);
        if (strncmp(sql + *p, symbols[i].text, len) == 0) {
            strcpy(t->text, symbols[i].text);
            t->type = symbols[i].type;
            *p += (int)len;
            return DB_OK;
        }
    }
    if (isprint((unsigned char)c)) {
        return error_set(err, DB_ERR_SYNTAX, *p, "unexpected character '%c'", c);
    }
    return error_set(err, DB_ERR_SYNTAX, *p, "unexpected byte 0x%02X", (unsigned char)c);
}

// 공백을 건너뛰고 토큰 하나를 읽는다. 입력이 끝났으면 TOK_EOF 를 만든다.
static DbStatus next_token(const char *sql, int *p, Token *t, DbError *err)
{
    while (isspace((unsigned char)sql[*p])) {
        (*p)++;
    }
    t->text[0] = '\0';
    t->int_value = 0;
    t->pos = *p;

    char c = sql[*p];
    if (c == '\0') {
        t->type = TOK_EOF;
        return DB_OK;
    }
    if (is_name_start(c)) {
        return read_word(sql, p, t, err);
    }
    if (is_digit(c) || (c == '-' && is_digit(sql[*p + 1]))) {
        return read_integer(sql, p, t, err);
    }
    if (c == '\'') {
        return read_string(sql, p, t, err);
    }
    return read_symbol(sql, p, t, err);
}

DbStatus tokenize_next(const char *sql, int *pos, Token *out, int max, int *count, DbError *err)
{
    int p = *pos;   // 실패하면 *pos 를 건드리지 않으려고 복사본으로 진행한다
    int n = 0;
    int done = 0;

    while (!done) {
        if (n >= max) {
            return error_set(err, DB_ERR_TOO_LONG, p, "statement has more than %d tokens", max);
        }
        Token *t = &out[n++];
        DbStatus status = next_token(sql, &p, t, err);
        if (status != DB_OK) {
            return status;
        }
        done = (t->type == TOK_EOF);

        // ; 는 문장의 끝이다. 뒤의 글자는 다음 호출이 읽도록 남겨 두고, 여기서 EOF 를 붙여 끝낸다.
        if (t->type == TOK_SEMICOLON) {
            if (n >= max) {
                return error_set(err, DB_ERR_TOO_LONG, p, "statement has more than %d tokens", max);
            }
            out[n++] = (Token){ .type = TOK_EOF, .pos = p };
            done = 1;
        }
    }
    *pos = p;
    *count = n;
    return DB_OK;
}
