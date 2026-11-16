#ifndef TOKEN_H
#define TOKEN_H

#include "error.h"

// 문장 하나의 토큰 수 한도. 마지막 TOK_EOF 까지 센 값이라 토큰 배열 크기로 그대로 쓴다.
#define MAX_TOKENS 64

typedef enum {
    TOK_INSERT, TOK_INTO, TOK_VALUES, TOK_SELECT, TOK_FROM, TOK_WHERE,
    TOK_IDENTIFIER, TOK_INTEGER, TOK_STRING,
    TOK_LPAREN, TOK_RPAREN, TOK_COMMA, TOK_SEMICOLON, TOK_STAR, TOK_EQ,
    TOK_EOF
} TokenType;

// 토큰은 원문을 가리키지 않고 값을 복사해 담는다.
// 문자열의 '' 를 ' 로 바꾸면 원문과 내용이 달라져서 포인터로는 표현할 수 없기 때문이다.
typedef struct {
    TokenType type;
    char      text[256];    // 이름 또는 문자열 값 (따옴표 뗀 것, '' 는 ' 로 바꾼 것)
    long long int_value;    // TOK_INTEGER 일 때
    int       pos;          // 원문에서 시작 위치
} Token;

// *pos 부터 문장 하나(; 까지 또는 입력 끝까지)를 토큰으로 만들고 *pos 를 그 뒤로 옮긴다.
// 마지막에 TOK_EOF 를 붙인다. 토큰의 pos 는 입력 전체 기준 위치다.
// 한 번에 문장 하나만 읽는 이유: Phase 2 에서 100만 문장을 넣을 때 전체를 토큰 배열로 들고 있지 않기 위해서다.
DbStatus tokenize_next(const char *sql, int *pos, Token *out, int max, int *count, DbError *err);

#endif
