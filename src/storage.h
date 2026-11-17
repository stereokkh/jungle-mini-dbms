#ifndef STORAGE_H
#define STORAGE_H

#include "db.h"
#include "error.h"
#include "statement.h"

// CSV 한 줄의 길이 한도(바이트). 끝의 줄바꿈까지 센 값이다.
// 읽는 버퍼는 '\0' 한 칸을 더해 char line[MAX_LINE_LEN + 1] 로 잡는다.
#define MAX_LINE_LEN 8192

// 저장소가 읽은 줄 하나마다 불린다. offset 은 그 줄의 시작 위치 (Phase 2 에서 B+ 트리에 넣는다).
// 0 이 아닌 값을 돌려주면 읽기를 멈춘다.
typedef int (*ScanCallback)(const Value *fields, int field_count, long offset, void *ctx);

// CSV 한 줄을 파일 끝에 붙이고 그 줄의 시작 offset 을 돌려준다
DbStatus storage_append(Table *t, const char *line, long *offset, DbError *err);

// 먼저 fflush(t->fp) 를 한 뒤, 읽기용으로 따로 열어 처음부터 한 줄씩 읽고 콜백을 부른다.
// 쓰기용 fp 는 버퍼에 아직 안 내려간 줄이 있을 수 있어서, 읽기 전에 비워야 방금 넣은 줄이 보인다.
DbStatus storage_scan(Db *db, Table *t, ScanCallback cb, void *ctx, DbError *err);

// offset 에서 시작하는 줄 하나만 읽어 콜백을 한 번 부른다 (인덱스로 찾은 행을 읽을 때 쓴다).
// storage_scan 처럼 먼저 fflush 하고 읽기용으로 따로 연다. 파일 앞부분은 읽지 않고 fseek 으로 건너뛴다.
// offset 에 온전한 줄이 없으면 DB_ERR_IO.
DbStatus storage_read_at(Db *db, Table *t, long offset, ScanCallback cb, void *ctx, DbError *err);

#endif
