#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "db.h"
#include "error.h"
#include "statement.h"

// execute 가 돌려주는 결과
typedef struct {
    long long inserted_id;   // INSERT 일 때 붙인 id
    int       row_count;     // SELECT 일 때 찾은 행 수
} ExecResult;

// SELECT 가 찾은 행 하나마다 불린다. CLI 는 여기서 출력하고, Phase 3 서버는 여기서 JSON 을 만든다.
// 테이블의 읽기 락을 잡은 상태이므로 콜백에서 같은 테이블의 쿼리를 다시 실행하지 않는다.
typedef void (*RowCallback)(const Value *fields, int field_count, void *ctx);

// 스키마 검사 후 INSERT 는 id 를 붙여 저장, SELECT 는 찾은 행을 콜백으로 전달
DbStatus execute(Db *db, const Statement *stmt, RowCallback on_row, void *ctx,
                 ExecResult *result, DbError *err);

#endif
