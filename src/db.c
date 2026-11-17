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

void db_index_row(Table *t, long long id, long offset)
{
    if (t->id_index_usable && bpt_insert(&t->id_index, id, offset) != BPT_OK) {
        // 할당 실패나 기존 CSV의 중복 ID가 있으면 전체 스캔으로 모든 행을 조회한다.
        bpt_free(t->id_index);
        t->id_index = NULL;
        t->id_index_usable = false;
    }
}

// 저장소가 스키마를 확인하므로 첫 필드는 항상 INT다. next_id에는 잠시 최대 ID를 담는다.
// CSV에서 읽은 기존 행을 B+트리에 등록하는 콜백
static int index_existing_row(const Value *fields, int field_count, long offset, void *ctx)
{
    Table *t = ctx;
    (void)field_count;
    long long id = fields[0].i;
    if (id > t->next_id) {
        t->next_id = id;
    }
    db_index_row(t, id, offset);
    return 0;
}

// 스키마를 읽고, CSV를 한 번 훑어 최대 ID와 인덱스를 구한 뒤 쓰기용 파일을 연다.
static DbStatus open_table(Db *db, const char *name, Table *t, DbError *err)
{
    char path[512];     // data_dir(255) + '/' + name(63) + ".schema" 가 항상 들어가는 크기
    DbStatus st;

    strcpy(t->name, name);  // 길이는 db_get_table 이 이미 검사했다
    t->fp = NULL;
    t->next_id = 0;
    t->id_index = NULL;
    t->id_index_usable = true;

    snprintf(path, sizeof path, "%s/%s.schema", db->data_dir, name);
    st = schema_load(path, &t->schema, err);
    if (st == DB_ERR_NO_TABLE) {
        // schema_load 는 파일 경로만 알기 때문에, 사용자에게 보일 메시지는 테이블 이름으로 다시 쓴다.
        return error_set(err, DB_ERR_NO_TABLE, -1, "table '%s' does not exist", name);
    }
    if (st != DB_OK) {
        return st;
    }

    st = storage_scan(db, t, index_existing_row, t, err);
    if (st != DB_OK) {
        goto fail;
    }
    // 가장 큰 id 가 long long 의 끝이면 +1 이 넘친다 (정의되지 않은 동작). 더하기 전에 막는다.
    if (t->next_id == LLONG_MAX) {
        st = error_set(err, DB_ERR_IO, -1, "table '%s' has no ids left", name);
        goto fail;
    }
    t->next_id++;

    snprintf(path, sizeof path, "%s/%s.csv", db->data_dir, name);
    t->fp = fopen(path, "ab");
    if (t->fp == NULL) {
        st = error_set(err, DB_ERR_IO, -1, "cannot open data file '%s'", path);
        goto fail;
    }
    return DB_OK;

fail:
    bpt_free(t->id_index);
    t->id_index = NULL;
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
        bpt_free(db->tables[i].id_index);
        db->tables[i].id_index = NULL;
    }
    db->table_count = 0;
}
