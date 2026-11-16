#include "db.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "storage.h"

// 오류를 채우고 그 코드를 그대로 돌려준다. SQL 위치와 상관없는 오류라서 pos 는 -1 이다.
static DbStatus fail(DbError *err, DbStatus code, const char *fmt, ...)
{
    char detail[200];
    va_list args;

    va_start(args, fmt);
    vsnprintf(detail, sizeof detail, fmt, args);
    va_end(args);

    err->code = code;
    err->pos = -1;
    snprintf(err->message, sizeof err->message, "Error: %s", detail);
    return code;
}

DbStatus db_open(const char *data_dir, Db *db, DbError *err)
{
    if (strlen(data_dir) >= sizeof db->data_dir) {
        return fail(err, DB_ERR_TOO_LONG, "data directory path is too long (limit is %d)",
                    (int)sizeof db->data_dir - 1);
    }
    strcpy(db->data_dir, data_dir);
    db->table_count = 0;
    return DB_OK;
}

// storage_scan 콜백: 지금까지 본 id 중 가장 큰 것을 ctx 에 남긴다.
// 저장소가 스키마와 맞는 줄만 넘겨주므로 첫 필드는 항상 INT 다.
static int keep_max_id(const Value *fields, int field_count, long offset, void *ctx)
{
    long long *max_id = ctx;

    (void)field_count;
    (void)offset;
    if (fields[0].i > *max_id) {
        *max_id = fields[0].i;
    }
    return 0;
}

// 빈 칸 t 에 테이블을 연다: 스키마 읽기 → next_id 정하기 → 데이터 파일을 "ab" 로 열기.
// 파일을 맨 마지막에 열어서, 중간에 실패해도 닫을 것이 없다.
static DbStatus open_table(Db *db, const char *name, Table *t, DbError *err)
{
    char path[512];     // data_dir(255) + '/' + name(63) + ".schema" 가 항상 들어가는 크기
    long long max_id = 0;
    DbStatus st;

    strcpy(t->name, name);  // 길이는 db_get_table 이 이미 검사했다
    t->fp = NULL;

    snprintf(path, sizeof path, "%s/%s.schema", db->data_dir, name);
    st = schema_load(path, &t->schema, err);
    if (st == DB_ERR_NO_TABLE) {
        // schema_load 는 파일 경로만 알기 때문에, 사용자에게 보일 메시지는 테이블 이름으로 다시 쓴다.
        return fail(err, DB_ERR_NO_TABLE, "table '%s' does not exist", name);
    }
    if (st != DB_OK) {
        return st;
    }

    st = storage_scan(db, t, keep_max_id, &max_id, err);
    if (st != DB_OK) {
        return st;
    }
    // 가장 큰 id 가 long long 의 끝이면 +1 이 넘친다 (정의되지 않은 동작). 더하기 전에 막는다.
    if (max_id == LLONG_MAX) {
        return fail(err, DB_ERR_IO, "table '%s' has no ids left", name);
    }
    t->next_id = max_id + 1;

    snprintf(path, sizeof path, "%s/%s.csv", db->data_dir, name);
    t->fp = fopen(path, "ab");
    if (t->fp == NULL) {
        return fail(err, DB_ERR_IO, "cannot open data file '%s'", path);
    }
    return DB_OK;
}

DbStatus db_get_table(Db *db, const char *name, Table **out, DbError *err)
{
    int max_tables = (int)(sizeof db->tables / sizeof db->tables[0]);

    for (int i = 0; i < db->table_count; i++) {
        if (strcmp(db->tables[i].name, name) == 0) {
            *out = &db->tables[i];
            return DB_OK;
        }
    }
    if (strlen(name) >= sizeof db->tables[0].name) {
        return fail(err, DB_ERR_TOO_LONG, "table name is too long (limit is %d)",
                    (int)sizeof db->tables[0].name - 1);
    }
    if (db->table_count >= max_tables) {
        return fail(err, DB_ERR_TOO_LONG, "too many open tables (limit is %d)", max_tables);
    }

    // 다 열린 뒤에만 개수를 늘린다. 실패한 칸은 다음에 다시 쓰인다.
    Table *t = &db->tables[db->table_count];
    DbStatus st = open_table(db, name, t, err);
    if (st != DB_OK) {
        return st;
    }
    db->table_count++;
    *out = t;
    return DB_OK;
}

void db_close(Db *db)
{
    for (int i = 0; i < db->table_count; i++) {
        fclose(db->tables[i].fp);
        db->tables[i].fp = NULL;
    }
    db->table_count = 0;
}
