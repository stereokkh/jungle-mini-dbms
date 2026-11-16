#ifndef STATEMENT_H
#define STATEMENT_H

#include "error.h"
#include "token.h"

typedef enum { STMT_SELECT, STMT_INSERT } StmtType;

// 값 하나. INT 와 TEXT 가 같은 자리를 나눠 쓰므로(union) 무엇이 들었는지 type 으로 구분한다.
typedef struct {
    enum { VAL_INT, VAL_STR } type;   // 꼬리표: 지금 뭐가 들었는지
    union {
        long long i;
        char      s[256];
    };
} Value;

typedef struct {
    char  column[64];
    char  op;             // '=' 만 지원
    Value value;
} Where;

// 문장 하나를 파싱한 결과.
// 크기가 고정된 배열만 써서 malloc/free 가 필요 없다. 해제를 잊을 일이 없게 하려는 것이다.
typedef struct {
    StmtType type;
    char     table[64];

    // SELECT: 가져올 컬럼
    int      select_all;          // * 이면 1
    char     columns[16][64];
    int      column_count;

    // INSERT: 넣을 값 (id 를 뺀 나머지, 순서 = 스키마 컬럼 순서)
    Value    values[16];
    int      value_count;

    // SELECT: 조건
    int      has_where;
    Where    where;
} Statement;

// 문장 하나의 토큰을 Statement 로 바꾼다. 끝은 [;] EOF 여야 한다.
DbStatus parse(const Token *toks, int count, Statement *stmt, DbError *err);

#endif
