#include "db.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "storage.h"

DbStatus db_open(const char *data_dir, Db *db, DbError *err)
{
    if (strlen(data_dir) >= sizeof db->data_dir) {
        return error_set(err, DB_ERR_TOO_LONG, -1, "data directory path is too long (limit is %d)",
                         (int)sizeof db->data_dir - 1);
    }
    strcpy(db->data_dir, data_dir);
    db->table_count = 0;
    return DB_OK;
}

// 테이블을 열면서 파일을 한 번 훑는 동안 모으는 것
typedef struct {
    long long max_id;         // 지금까지 본 id 중 가장 큰 것
    BPTree   *index;          // (id, offset) 을 넣을 트리
    int       out_of_memory;  // 트리에 넣다가 메모리가 모자랐으면 1
} LoadState;

// storage_scan 콜백: 줄마다 가장 큰 id 를 기억하고, (id, 그 줄의 offset) 을 인덱스에 넣는다.
// 저장소가 스키마와 맞는 줄만 넘겨주므로 첫 필드는 항상 INT 다.
static int load_row(const Value *fields, int field_count, long offset, void *ctx)
{
    LoadState *load = ctx;
    long long id = fields[0].i;

    (void)field_count;
    if (id > load->max_id) {
        load->max_id = id;
    }
    if (bptree_insert(load->index, id, offset) != 0) {
        load->out_of_memory = 1;
        return 1;   // 그만 읽는다
    }
    return 0;
}

// 스키마를 읽은 뒤의 나머지: 파일을 훑어 next_id 와 인덱스 만들기 → 데이터 파일을 "ab" 로 열기.
// 실패하면 만들던 인덱스는 부른 쪽(open_table)이 해제한다.
static DbStatus load_table(Db *db, Table *t, DbError *err)
{
    char path[512];     // data_dir(255) + '/' + name(63) + ".csv" 가 항상 들어가는 크기
    LoadState load = { 0, t->index, 0 };

    DbStatus st = storage_scan(db, t, load_row, &load, err);
    if (st != DB_OK) {
        return st;
    }
    if (load.out_of_memory) {
        return error_set(err, DB_ERR_IO, -1, "out of memory while indexing table '%s'", t->name);
    }
    // 가장 큰 id 가 long long 의 끝이면 +1 이 넘친다 (정의되지 않은 동작). 더하기 전에 막는다.
    if (load.max_id == LLONG_MAX) {
        return error_set(err, DB_ERR_IO, -1, "table '%s' has no ids left", t->name);
    }
    t->next_id = load.max_id + 1;

    snprintf(path, sizeof path, "%s/%s.csv", db->data_dir, t->name);
    t->fp = fopen(path, "ab");
    if (t->fp == NULL) {
        return error_set(err, DB_ERR_IO, -1, "cannot open data file '%s'", path);
    }
    return DB_OK;
}

// 빈 칸 t 에 테이블을 연다: 스키마 읽기 → 빈 인덱스 만들기 → load_table.
// 파일을 맨 마지막에 열어서, 중간에 실패하면 인덱스만 해제하면 된다.
static DbStatus open_table(Db *db, const char *name, Table *t, DbError *err)
{
    char path[512];     // data_dir(255) + '/' + name(63) + ".schema" 가 항상 들어가는 크기
    DbStatus st;

    strcpy(t->name, name);  // 길이는 db_get_table 이 이미 검사했다
    t->fp = NULL;
    t->index = NULL;

    snprintf(path, sizeof path, "%s/%s.schema", db->data_dir, name);
    st = schema_load(path, &t->schema, err);
    if (st == DB_ERR_NO_TABLE) {
        // schema_load 는 파일 경로만 알기 때문에, 사용자에게 보일 메시지는 테이블 이름으로 다시 쓴다.
        return error_set(err, DB_ERR_NO_TABLE, -1, "table '%s' does not exist", name);
    }
    if (st != DB_OK) {
        return st;
    }

    t->index = bptree_create();
    if (t->index == NULL) {
        return error_set(err, DB_ERR_IO, -1, "out of memory while indexing table '%s'", name);
    }
    st = load_table(db, t, err);
    if (st != DB_OK) {
        bptree_destroy(t->index);
        t->index = NULL;
    }
    return st;
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
        return error_set(err, DB_ERR_TOO_LONG, -1, "table name is too long (limit is %d)",
                         (int)sizeof db->tables[0].name - 1);
    }
    if (db->table_count >= max_tables) {
        return error_set(err, DB_ERR_TOO_LONG, -1, "too many open tables (limit is %d)", max_tables);
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
        bptree_destroy(db->tables[i].index);
        db->tables[i].index = NULL;
    }
    db->table_count = 0;
}
