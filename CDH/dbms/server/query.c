#include "query.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "executor.h"
#include "statement.h"
#include "token.h"

// ---------- 늘어나는 문자열 버퍼 ----------
// SELECT 결과는 몇 줄일지 모르므로 크기를 미리 정할 수 없다. 모자라면 두 배로 늘린다.

typedef struct {
    char  *data;
    size_t len;      // 쓴 글자 수 ('\0' 제외)
    size_t cap;      // 할당한 크기
    int    failed;   // 메모리 할당에 한 번이라도 실패했으면 1 (이후 쓰기는 무시)
} Buf;

// [역할] n 바이트를 버퍼 끝에 붙인다. 자리가 모자라면 realloc 으로 두 배씩 늘린다.
static void buf_add(Buf *b, const char *s, size_t n)
{
    if (b->failed) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (b->len + n + 1 > cap) cap *= 2;
        char *p = realloc(b->data, cap);
        if (p == NULL) {
            b->failed = 1;
            return;
        }
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

// [역할] C 문자열 하나를 붙인다.
static void buf_str(Buf *b, const char *s)
{
    buf_add(b, s, strlen(s));
}

// [역할] 정수 하나를 글자로 바꿔 붙인다.
static void buf_int(Buf *b, long long v)
{
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, "%lld", v);
    buf_add(b, tmp, (size_t)n);
}

// [역할] s 를 JSON 문자열("...")로 붙인다.
// [기능] " 와 \ 앞에 \ 를 붙이고, 줄바꿈 같은 제어 문자는 \n 이나 \u00XX 로 바꾼다.
//        사용자가 넣은 이름에 " 가 있어도 JSON 이 깨지지 않게 하는 핵심 처리다.
static void buf_json_str(Buf *b, const char *s)
{
    buf_add(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            char esc[2] = { '\\', (char)*p };
            buf_add(b, esc, 2);
        } else if (*p == '\n') {
            buf_add(b, "\\n", 2);
        } else if (*p < 0x20) {
            char esc[8];
            snprintf(esc, sizeof esc, "\\u%04x", *p);
            buf_add(b, esc, 6);
        } else {
            buf_add(b, (const char *)p, 1);   // 한글(UTF-8) 등은 그대로
        }
    }
    buf_add(b, "\"", 1);
}

// ---------- 엔진 결과 → JSON ----------

// SELECT 콜백에 넘기는 상태
typedef struct {
    Buf *b;
    int  rows;   // 지금까지 쓴 행 수 (첫 행 앞에는 쉼표를 안 쓰려고)
} RowCtx;

// [역할] 엔진이 SELECT 로 찾은 행마다 불러 주는 콜백. 행 하나를 [1,"Kim",20] 모양으로 붙인다.
static void on_row(const Value *fields, int count, void *ctx)
{
    RowCtx *rc = ctx;

    buf_str(rc->b, rc->rows++ ? ",[" : "[");
    for (int i = 0; i < count; i++) {
        if (i) buf_str(rc->b, ",");
        if (fields[i].type == VAL_INT) buf_int(rc->b, fields[i].i);
        else buf_json_str(rc->b, fields[i].s);
    }
    buf_str(rc->b, "]");
}

// [역할] SELECT 결과의 컬럼 이름 목록 ["id","name"] 을 붙인다.
// [기능] SELECT * 이면 테이블 스키마 순서, 아니면 SQL 에 적은 순서.
static void add_columns(Buf *b, Db *db, const Statement *stmt)
{
    buf_str(b, "\"columns\":[");
    if (stmt->select_all) {
        Table *t;
        DbError err;
        // execute 가 이미 테이블을 열었으므로 여기서는 실패하지 않는다.
        if (db_get_table(db, stmt->table, &t, &err) == DB_OK) {
            for (int i = 0; i < t->schema.count; i++) {
                if (i) buf_str(b, ",");
                buf_json_str(b, t->schema.cols[i].name);
            }
        }
    } else {
        for (int i = 0; i < stmt->column_count; i++) {
            if (i) buf_str(b, ",");
            buf_json_str(b, stmt->columns[i]);
        }
    }
    buf_str(b, "]");
}

// [역할] 엔진 오류 코드를 HTTP 상태 코드로 바꾼다.
// [기능] 사용자 SQL 이 틀렸으면 400, 테이블이 없으면 404, 서버 쪽 파일 문제면 500.
static int http_status_of(DbStatus st)
{
    switch (st) {
    case DB_ERR_NO_TABLE: return 404;
    case DB_ERR_SCHEMA:
    case DB_ERR_IO:       return 500;
    default:              return 400;   // SYNTAX, TOO_LONG, NO_COLUMN, COLUMN_COUNT, TYPE
    }
}

// [역할] 토큰으로 된 문장 하나를 파싱·실행하고, 결과 객체 하나를 b 에 붙인다.
// [순서] parse → execute (SELECT 면 on_row 가 행을 붙임) → statement_free
static DbStatus run_statement(Db *db, const Token *toks, int count, Buf *b, DbError *err)
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

            buf_str(b, "{\"type\":\"insert\",\"id\":");
            buf_int(b, result.inserted_id);
            buf_str(b, "}");
        }
    } else {
        // 행은 execute 도중 콜백으로 붙으므로, rows 배열을 먼저 연다.
        // 실패하면 아래 query_run 이 버퍼 전체를 버리므로 반쯤 쓴 내용은 상관없다.
        RowCtx rc = { b, 0 };
        buf_str(b, "{\"type\":\"select\",\"rows\":[");
        st = execute(db, &stmt, on_row, &rc, &result, err);
        if (st == DB_OK) {
            buf_str(b, "],");
            add_columns(b, db, &stmt);
            buf_str(b, ",\"row_count\":");
            buf_int(b, result.row_count);
            buf_str(b, "}");
        }
    }
    statement_free(&stmt);   // WHERE 조건 트리 해제 (실패해도 해제)
    return st;
}

// [역할] 오류 응답 JSON {"ok":false,"error":"...","executed":N} 을 만든다.
static char *error_json(const char *message, int executed)
{
    Buf b = {0};
    buf_str(&b, "{\"ok\":false,\"error\":");
    buf_json_str(&b, message);
    buf_str(&b, ",\"executed\":");
    buf_int(&b, executed);
    buf_str(&b, "}");
    if (b.failed) {
        free(b.data);
        return NULL;
    }
    return b.data;
}

char *query_run(Db *db, const char *sql, int *status)
{
    Token toks[MAX_TOKENS];
    DbError err;
    Buf b = {0};
    int pos = 0, count = 0, executed = 0;
    DbStatus st = DB_OK;

    buf_str(&b, "{\"ok\":true,\"results\":[");

    // 문장을 하나씩 토큰으로 만들어 실행한다 (CLI 의 run_sql 과 같은 방식).
    for (;;) {
        st = tokenize_next(sql, &pos, toks, MAX_TOKENS, &count, &err);
        if (st != DB_OK) break;
        if (count == 1) break;                          // EOF 토큰 하나뿐 = 끝
        if (toks[0].type == TOK_SEMICOLON) continue;    // 빈 문장 ";"

        if (executed) buf_str(&b, ",");
        st = run_statement(db, toks, count, &b, &err);
        if (st != DB_OK) break;
        executed++;
    }

    if (st != DB_OK) {
        free(b.data);
        *status = http_status_of(st);
        char *json = error_json(err.message, executed);
        if (json == NULL) *status = 500;
        return json;
    }
    if (executed == 0) {
        free(b.data);
        *status = 400;
        return error_json("no SQL statement", 0);
    }

    buf_str(&b, "]}");
    if (b.failed) {               // 결과가 너무 커서 메모리가 모자랐다
        free(b.data);
        *status = 500;
        return NULL;
    }
    *status = 200;
    return b.data;
}
