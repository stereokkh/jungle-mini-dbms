#ifndef DB_H
#define DB_H

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>

#include "bptree.h"
#include "error.h"
#include "schema.h"

// 열어 둔 테이블 하나
typedef struct {
    char      name[64];
    Schema    schema;
    pthread_rwlock_t lock; // SELECT 는 읽기 락, INSERT 는 쓰기 락을 잡는다
    FILE     *fp;         // "ab" 로 열어 둔 데이터 파일. INSERT 마다 열고 닫지 않는다
    bool      write_failed; // 쓰기 실패 후에는 파일과 인덱스가 일치한다고 볼 수 없다
    long long next_id;    // 다음 INSERT 에 붙일 id
    BPTree   *index;      // id → 그 행의 파일 offset. 파일에 저장하지 않고 테이블을 열 때마다 다시 만든다
    int       index_usable; // 1 이면 index 를 믿고 `WHERE id = N` 을 찾는다.
                            // 메모리가 모자라 행 하나라도 못 넣었으면 0 이고, index 는 NULL 이다
} Table;

// 실행 중인 DB 하나. 전역 변수 대신 이것을 함수에 넘긴다
typedef struct {
    char  data_dir[256];
    Table tables[8];
    int   table_count;
    pthread_mutex_t tables_mutex; // tables 와 table_count 의 조회 및 등록을 보호한다
} Db;

// DB 준비 (테이블은 처음 쓸 때 연다)
DbStatus db_open(const char *data_dir, Db *db, DbError *err);

// 열린 테이블을 찾고, 없으면 초기화한 뒤 등록한다. 반환된 테이블은 db_close 까지 유효하다.
DbStatus db_get_table(Db *db, const char *name, Table **out, DbError *err);

// 인덱스를 해제하고 index_usable 을 0 으로 만든다. 이후 이 테이블은 선형 탐색으로만 조회한다.
void db_drop_index(Table *t);

// 모든 요청이 끝난 뒤 호출한다. 열어 둔 파일, 인덱스, 락을 해제한다.
void db_close(Db *db);

#endif
