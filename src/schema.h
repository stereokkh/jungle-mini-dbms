#ifndef SCHEMA_H
#define SCHEMA_H

#include "error.h"

typedef enum { COL_INT, COL_TEXT } ColumnType;

typedef struct {
    char       name[64];
    ColumnType type;
} Column;

typedef struct {
    Column cols[16];
    int    count;
} Schema;

// 스키마 파일 읽기. 첫 컬럼이 id,INT 가 아니면 DB_ERR_SCHEMA.
// id 를 엔진이 붙이므로 첫 컬럼이 id 라는 약속이 깨진 파일은 처음부터 받지 않는다.
DbStatus schema_load(const char *path, Schema *out, DbError *err);

#endif
