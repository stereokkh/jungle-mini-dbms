#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"
#include "executor.h"
#include "server.h"
#include "statement.h"
#include "token.h"

// 테이블 파일을 찾는 곳. 프로그램을 실행한 디렉터리 기준이다.
#define DATA_DIR "data"

// 파일 전체를 읽어 '\0' 으로 끝나는 문자열로 돌려준다. 실패하면 NULL. 다 쓰면 free 한다.
// 토크나이저가 위치를 int 로 세기 때문에 그보다 큰 파일은 받지 않는다.
static char *read_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    char *text = NULL;

    if (fp == NULL) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) == 0) {
        long size = ftell(fp);
        if (size >= 0 && size < INT_MAX && fseek(fp, 0, SEEK_SET) == 0) {
            text = malloc((size_t)size + 1);
        }
        if (text != NULL && fread(text, 1, (size_t)size, fp) != (size_t)size) {
            free(text);
            text = NULL;
        }
        if (text != NULL) {
            text[size] = '\0';
        }
    }
    fclose(fp);
    return text;
}

// 데이터 파일과 같은 모양으로 출력한다: TEXT 는 큰따옴표로 감싸고 안의 " 는 "" 로 쓴다.
static void print_value(const Value *v)
{
    if (v->type == VAL_INT) {
        printf("%lld", v->i);
        return;
    }
    putchar('"');
    for (const char *p = v->s; *p != '\0'; p++) {
        if (*p == '"') {
            putchar('"');
        }
        putchar(*p);
    }
    putchar('"');
}

// execute 가 SELECT 로 찾은 행마다 부르는 콜백. Phase 3 서버는 이 자리에서 JSON 을 만든다.
static void print_row(const Value *fields, int field_count, void *ctx)
{
    (void)ctx;
    for (int i = 0; i < field_count; i++) {
        if (i > 0) {
            putchar(',');
        }
        print_value(&fields[i]);
    }
    putchar('\n');
}

// 토큰으로 만든 문장 하나를 파싱하고 실행해서 결과를 출력한다.
static DbStatus run_statement(Db *db, const Token *toks, int count, DbError *err)
{
    Statement stmt;
    ExecResult result;
    DbStatus st;

    if ((st = parse(toks, count, &stmt, err)) != DB_OK) return st;
    st = execute(db, &stmt, print_row, NULL, &result, err);
    statement_free(&stmt);  // 실행이 실패해도 조건 트리는 해제한다
    if (st != DB_OK) return st;

    if (stmt.type == STMT_INSERT) {
        printf("OK (id=%lld)\n", result.inserted_id);
    } else {
        printf("(%d rows)\n", result.row_count);
    }
    return DB_OK;
}

// SQL 문자열의 문장을 하나씩 차례로 실행한다. 오류가 난 문장에서 멈춘다.
// 전체를 먼저 토큰으로 만들지 않고 문장 하나씩 처리해서, 문장이 100만 개여도 메모리는 그대로다.
static DbStatus run_sql(Db *db, const char *sql, DbError *err)
{
    Token toks[MAX_TOKENS];
    int pos = 0;
    int count = 0;

    for (;;) {
        DbStatus st = tokenize_next(sql, &pos, toks, MAX_TOKENS, &count, err);
        if (st != DB_OK) {
            return st;
        }
        if (count == 1) {
            return DB_OK;   // EOF 하나뿐: 더 읽을 문장이 없다
        }
        if (toks[0].type == TOK_SEMICOLON) {
            continue;       // ; 만 있는 빈 문장
        }
        st = run_statement(db, toks, count, err);
        if (st != DB_OK) {
            return st;
        }
    }
}

// --server 모드: DB 를 한 번만 열고, 프로세스가 살아 있는 동안 계속 요청을 받는다.
// CLI 처럼 요청마다 db_open 하면 매번 데이터 파일 전체를 읽고 B+ 트리를 다시 만든다.
static int run_server(int argc, char **argv)
{
    Db db;
    DbError err;
    int port = (argc >= 3) ? atoi(argv[2]) : 8080;
    int workers = (argc >= 4) ? atoi(argv[3]) : 4;

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Error: bad port '%s'\n", argv[2]);
        return 1;
    }
    if (db_open(DATA_DIR, &db, &err) != DB_OK) {
        fprintf(stderr, "%s\n", err.message);
        return 1;
    }
    int rc = server_run(&db, port, workers);   // 정상이면 돌아오지 않는다 (Ctrl+C 로 끈다)
    db_close(&db);
    return rc == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    Db db;
    DbError err;

    if (argc >= 2 && strcmp(argv[1], "--server") == 0) {
        return run_server(argc, argv);
    }
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <file.sql>\n"
                        "       %s --server [port] [workers]\n", argv[0], argv[0]);
        return 1;
    }
    char *sql = read_file(argv[1]);
    if (sql == NULL) {
        fprintf(stderr, "Error: cannot read file '%s'\n", argv[1]);
        return 1;
    }

    DbStatus st = db_open(DATA_DIR, &db, &err);
    if (st == DB_OK) {
        st = run_sql(&db, sql, &err);
        db_close(&db);
    }
    free(sql);

    if (st != DB_OK) {
        fflush(stdout);     // 오류 전에 성공한 문장의 출력이 먼저 보이게 한다
        fprintf(stderr, "%s\n", err.message);
        return 1;
    }
    return 0;
}
