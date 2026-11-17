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

typedef enum { OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE } CompareOp;   // =  !=  <  >  <=  >=

typedef enum {
    COND_COMPARE,   // 잎: 컬럼 op 값
    COND_AND,       // 가지: left AND right
    COND_OR,        // 가지: left OR right
    COND_NOT        // 가지: NOT left (right 는 쓰지 않는다)
} CondType;

// WHERE 조건 트리의 노드 하나.
// `a = 1 AND (b < 2 OR NOT c = 3)` 처럼 조건이 몇 겹으로든 묶일 수 있어서,
// 고정 크기 배열이 아니라 malloc 한 노드를 포인터로 잇는다.
// 잎(COND_COMPARE)은 column/op/value 를 쓰고, 가지는 left/right 를 쓴다.
typedef struct Cond {
    CondType     type;

    // COND_COMPARE
    char         column[64];
    CompareOp    op;
    Value        value;

    // COND_AND, COND_OR, COND_NOT
    struct Cond *left;
    struct Cond *right;
} Cond;

// 문장 하나를 파싱한 결과.
// WHERE 조건 트리(where)만 malloc 으로 만들고 나머지는 크기가 고정된 배열이다.
// 그래서 parse 가 성공한 Statement 는 다 쓴 뒤 statement_free 로 정리해야 한다.
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

    // SELECT: 조건 트리의 뿌리. WHERE 가 없으면 NULL
    Cond    *where;
} Statement;

// 문장 하나의 토큰을 Statement 로 바꾼다. 끝은 [;] EOF 여야 한다.
// 실패하면 만들던 조건 트리를 스스로 해제하고 stmt->where 를 NULL 로 둔다.
DbStatus parse(const Token *toks, int count, Statement *stmt, DbError *err);

// parse 가 만든 조건 트리를 해제하고 stmt->where 를 NULL 로 만든다. 두 번 불러도 안전하다.
void statement_free(Statement *stmt);

#endif
