#include "query.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "executor.h"
#include "json.h"
#include "rpc.h"
#include "statement.h"
#include "token.h"

// ---------- 락 (3단계) ----------
//
// 엔진(src/)은 스레드를 모른다. 그래서 엔진을 부르는 이 파일에서 락을 잡는다.
//
//   g_catalog            Db.tables 목록(어떤 테이블이 열려 있나)을 지킨다.
//                        평소엔 읽기 락. 처음 보는 테이블을 열어 목록에 넣을 때만 쓰기 락.
//   g_table_locks[i]     Db.tables[i] 의 내용(next_id, B+ 트리, 파일)을 지킨다.
//                        SELECT = 읽기 락 → 여러 SELECT 가 동시에 돈다.
//                        INSERT = 쓰기 락 → 그 테이블에서 혼자 돈다.
//
// SELECT 도 엔진 안에서 fflush(t->fp) 를 부른다. 하지만 INSERT 가 응답 전에 이미 비워 두어
// 버퍼가 비어 있고, FILE 은 자체 락이 있어서 읽기 락끼리 함께 불러도 안전하다.
// (SELECT 는 읽을 때마다 파일을 따로 fopen 하므로 읽기 위치도 서로 섞이지 않는다.)
//
// 잡는 순서는 항상 g_catalog → 테이블 락. 순서가 하나라서 서로 기다리다 멈추는(교착) 일이 없다.

#define MAX_OPEN_TABLES (sizeof ((Db *)0)->tables / sizeof ((Db *)0)->tables[0])

static pthread_rwlock_t g_catalog;
static pthread_rwlock_t g_table_locks[MAX_OPEN_TABLES];
static pthread_once_t   g_locks_once = PTHREAD_ONCE_INIT;

// [역할] 락들을 처음 한 번만 초기화한다. (pthread_once 가 여러 스레드 중 딱 한 번만 부른다)
// [기능] "쓰기 우선" 속성을 준다. 기본 rwlock 은 읽기가 끊이지 않으면 INSERT 가 영원히 못 들어간다.
static void init_locks(void)
{
    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
    pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);

    pthread_rwlock_init(&g_catalog, &attr);
    for (size_t i = 0; i < MAX_OPEN_TABLES; i++) {
        pthread_rwlock_init(&g_table_locks[i], &attr);
    }
    pthread_rwlockattr_destroy(&attr);
}

// [역할] 이미 열린 테이블 목록에서 name 을 찾는다. g_catalog 를 잡은 상태에서 불러야 한다.
// [반환] 목록의 위치(0..), 아직 안 열렸으면 -1
static int find_open_table(const Db *db, const char *name)
{
    for (int i = 0; i < db->table_count; i++) {
        if (strcmp(db->tables[i].name, name) == 0) return i;
    }
    return -1;
}

// [역할] 문장 하나를 실행하기 전에 필요한 락을 잡는다.
// [순서] g_catalog 읽기 락 → 테이블 찾기
//        → 없으면: 읽기 락 풀고 쓰기 락 → db_get_table 로 열어 목록에 등록 → 풀고 처음부터 다시
//        → 있으면: 그 테이블 락을 SELECT 면 읽기, INSERT 면 쓰기로 잡는다
// [반환] DB_OK 면 g_catalog(읽기)와 테이블 락을 잡은 채로 돌아온다 (*slot = 테이블 위치).
//        실패하면(테이블 없음 등) 락을 하나도 잡지 않은 채 오류를 돌려준다.
static DbStatus lock_for(Db *db, const Statement *stmt, int *slot, DbError *err)
{
    pthread_once(&g_locks_once, init_locks);
    for (;;) {
        pthread_rwlock_rdlock(&g_catalog);
        int i = find_open_table(db, stmt->table);
        if (i >= 0) {
            if (stmt->type == STMT_INSERT) pthread_rwlock_wrlock(&g_table_locks[i]);
            else pthread_rwlock_rdlock(&g_table_locks[i]);
            *slot = i;
            return DB_OK;
        }
        pthread_rwlock_unlock(&g_catalog);

        // 처음 쓰는 테이블: 목록을 바꾸므로 혼자 들어간다. 그사이 다른 스레드가 먼저 열었으면
        // db_get_table 이 목록에서 찾기만 하고 끝난다.
        Table *t;
        pthread_rwlock_wrlock(&g_catalog);
        DbStatus st = db_get_table(db, stmt->table, &t, err);
        pthread_rwlock_unlock(&g_catalog);
        if (st != DB_OK) return st;
    }
}

// [역할] lock_for 가 잡은 락을 거꾸로 푼다.
static void unlock_for(int slot)
{
    pthread_rwlock_unlock(&g_table_locks[slot]);
    pthread_rwlock_unlock(&g_catalog);
}

// SELECT 콜백에 넘기는 상태
typedef struct {
    JsonBuf *b;
    int      rows;   // 지금까지 쓴 행 수 (첫 행 앞에는 쉼표를 안 쓰려고)
} RowCtx;

// [역할] 엔진이 SELECT 로 찾은 행마다 불러 주는 콜백. 행 하나를 [1,"Kim",20] 모양으로 붙인다.
static void on_row(const Value *fields, int count, void *ctx)
{
    RowCtx *rc = ctx;

    jb_str(rc->b, rc->rows++ ? ",[" : "[");
    for (int i = 0; i < count; i++) {
        if (i) jb_str(rc->b, ",");
        if (fields[i].type == VAL_INT) jb_int(rc->b, fields[i].i);
        else jb_json_str(rc->b, fields[i].s);
    }
    jb_str(rc->b, "]");
}

// [역할] SELECT 결과의 컬럼 이름 목록 ["id","name"] 을 붙인다.
// [기능] SELECT * 이면 테이블 스키마 순서, 아니면 SQL 에 적은 순서.
static void add_columns(JsonBuf *b, Db *db, const Statement *stmt)
{
    jb_str(b, "\"columns\":[");
    if (stmt->select_all) {
        Table *t;
        DbError err;
        // execute 가 이미 테이블을 열었으므로 여기서는 실패하지 않는다.
        if (db_get_table(db, stmt->table, &t, &err) == DB_OK) {
            for (int i = 0; i < t->schema.count; i++) {
                if (i) jb_str(b, ",");
                jb_json_str(b, t->schema.cols[i].name);
            }
        }
    } else {
        for (int i = 0; i < stmt->column_count; i++) {
            if (i) jb_str(b, ",");
            jb_json_str(b, stmt->columns[i]);
        }
    }
    jb_str(b, "]");
}

// [역할] 엔진 오류 코드를 JSON-RPC 오류 코드로 바꾼다.
// [기능] 테이블이 없으면 -32002, 서버 쪽 파일 문제면 -32003, 나머지(사용자 SQL 잘못)는 -32001.
static int rpc_code_of(DbStatus st)
{
    switch (st) {
    case DB_ERR_NO_TABLE: return RPC_TABLE_NOT_FOUND;
    case DB_ERR_SCHEMA:
    case DB_ERR_IO:       return RPC_STORAGE_ERROR;
    default:              return RPC_SQL_ERROR;   // SYNTAX, TOO_LONG, NO_COLUMN, COLUMN_COUNT, TYPE
    }
}

// [역할] 토큰으로 된 문장 하나를 파싱·실행하고, 결과 객체 하나를 b 에 붙인다.
// [순서] parse (락 없이) → lock_for → execute (SELECT 면 on_row 가 행을 붙임) → unlock_for
//        → statement_free
static DbStatus run_statement(Db *db, const Token *toks, int count, JsonBuf *b, DbError *err)
{
    Statement stmt;
    ExecResult result;
    int slot;
    DbStatus st = parse(toks, count, &stmt, err);
    if (st != DB_OK) return st;

    // 파싱은 공유 자료를 안 건드리므로 락 밖에서 한다. 락은 엔진을 만지는 동안만.
    st = lock_for(db, &stmt, &slot, err);
    if (st != DB_OK) {
        statement_free(&stmt);
        return st;
    }

    if (stmt.type == STMT_INSERT) {
        st = execute(db, &stmt, NULL, NULL, &result, err);
        if (st == DB_OK) {
            // 엔진은 쓰기를 메모리 버퍼에 모아 둔다. 서버는 오래 떠 있으므로
            // 응답 전에 디스크로 내려서, 서버가 갑자기 죽어도 "OK 받은 INSERT" 는 남게 한다.
            Table *t;
            if (db_get_table(db, stmt.table, &t, err) == DB_OK) fflush(t->fp);

            jb_str(b, "{\"type\":\"insert\",\"id\":");
            jb_int(b, result.inserted_id);
            jb_str(b, "}");
        }
    } else {
        // 행은 execute 도중 콜백으로 붙으므로, rows 배열을 먼저 연다.
        // 실패하면 query_run 이 버퍼 전체를 버리므로 반쯤 쓴 내용은 상관없다.
        RowCtx rc = { b, 0 };
        jb_str(b, "{\"type\":\"select\",\"rows\":[");
        st = execute(db, &stmt, on_row, &rc, &result, err);
        if (st == DB_OK) {
            jb_str(b, "],");
            add_columns(b, db, &stmt);
            jb_str(b, ",\"row_count\":");
            jb_int(b, result.row_count);
            jb_str(b, "}");
        }
    }
    unlock_for(slot);
    statement_free(&stmt);   // WHERE 조건 트리 해제 (실패해도 해제)
    return st;
}

// [역할] 실패 정보를 채우고 NULL 을 돌려준다. `return fail(...)` 한 줄로 빠져나가기 위한 모양.
static char *fail(QueryError *qe, int code, const char *message, int executed)
{
    qe->code = code;
    snprintf(qe->message, sizeof qe->message, "%s", message);
    qe->executed = executed;
    return NULL;
}

char *query_run(Db *db, const char *sql, QueryError *qe)
{
    Token toks[MAX_TOKENS];
    DbError err;
    JsonBuf b = {0};
    int pos = 0, count = 0, executed = 0;
    DbStatus st = DB_OK;

    jb_str(&b, "{\"results\":[");

    // 문장을 하나씩 토큰으로 만들어 실행한다 (CLI 의 run_sql 과 같은 방식).
    for (;;) {
        st = tokenize_next(sql, &pos, toks, MAX_TOKENS, &count, &err);
        if (st != DB_OK) break;
        if (count == 1) break;                          // EOF 토큰 하나뿐 = 끝
        if (toks[0].type == TOK_SEMICOLON) continue;    // 빈 문장 ";"

        if (executed) jb_str(&b, ",");
        st = run_statement(db, toks, count, &b, &err);
        if (st != DB_OK) break;
        executed++;
    }

    if (st != DB_OK) {
        free(jb_finish(&b));
        return fail(qe, rpc_code_of(st), err.message, executed);
    }
    if (executed == 0) {
        free(jb_finish(&b));
        return fail(qe, RPC_INVALID_PARAMS, "no SQL statement", 0);
    }

    jb_str(&b, "]}");
    char *json = jb_finish(&b);
    if (json == NULL) {   // 결과가 너무 커서 메모리가 모자랐다
        return fail(qe, RPC_INTERNAL_ERROR, "out of memory", executed);
    }
    return json;
}
