#include "query.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "executor.h"
#include "json.h"
#include "rpc.h"
#include "statement.h"
#include "token.h"

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
// [순서] parse → execute (SELECT 면 on_row 가 행을 붙임) → statement_free
static DbStatus run_statement(Db *db, const Token *toks, int count, JsonBuf *b, DbError *err)
{
    Statement stmt;
    ExecResult result;
    DbStatus st = parse(toks, count, &stmt, err);
    if (st != DB_OK) return st;

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
