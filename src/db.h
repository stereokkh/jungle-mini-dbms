#ifndef DB_H
#define DB_H

#include <stdio.h>

#include "bptree.h"
#include "error.h"
#include "schema.h"

// 열어 둔 테이블 하나
typedef struct {
    char      name[64];
    Schema    schema;
    FILE     *fp;         // "ab" 로 열어 둔 데이터 파일. INSERT 마다 열고 닫지 않는다
    long long next_id;    // 다음 INSERT 에 붙일 id
    BPTree   *index;      // id → 그 행의 파일 offset. 파일에 저장하지 않고 테이블을 열 때마다 다시 만든다
} Table;

// 실행 중인 DB 하나. 전역 변수 대신 이것을 함수에 넘긴다
typedef struct {
    char  data_dir[256];
    Table tables[8];
    int   table_count;
} Db;

// DB 준비 (테이블은 처음 쓸 때 연다)
DbStatus db_open(const char *data_dir, Db *db, DbError *err);

// 이미 열었으면 그것을 돌려준다.
// 처음이면 스키마를 읽고, 데이터 파일을 훑어 next_id 를 정하고 인덱스를 만들고, 파일을 "ab" 로 열어 둔다.
DbStatus db_get_table(Db *db, const char *name, Table **out, DbError *err);

// 열어 둔 파일을 모두 닫고 인덱스를 해제한다
void db_close(Db *db);

#endif
