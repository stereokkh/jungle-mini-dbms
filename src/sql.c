#include "sql.h"

#include <ctype.h>

DbStatus execute_tokens(Db *db, const Token *tokens, int count, RowCallback on_row, void *ctx,
                       StmtType *type, ExecResult *result, DbError *err)
{
    Statement stmt;
    DbStatus status = parse(tokens, count, &stmt, err);
    if (status != DB_OK) {
        return status;
    }

    *type = stmt.type;
    status = execute(db, &stmt, on_row, ctx, result, err);
    statement_free(&stmt);
    return status;
}

DbStatus run_one_sql(Db *db, const char *sql, RowCallback on_row, void *ctx,
                     StmtType *type, ExecResult *result, DbError *err)
{
    Token tokens[MAX_TOKENS];
    int pos = 0;
    int count = 0;
    DbStatus status = tokenize_next(sql, &pos, tokens, MAX_TOKENS, &count, err);
    if (status != DB_OK) {
        return status;
    }
    if (tokens[0].type == TOK_EOF || tokens[0].type == TOK_SEMICOLON) {
        return error_set(err, DB_ERR_SYNTAX, tokens[0].pos, "expected one SQL statement");
    }

    // 토크나이저가 문자열 리터럴 안의 세미콜론을 처리했으므로 남은 입력만 확인한다.
    while (isspace((unsigned char)sql[pos])) {
        pos++;
    }
    if (sql[pos] != '\0') {
        return error_set(err, DB_ERR_SYNTAX, pos, "expected only one SQL statement");
    }
    return execute_tokens(db, tokens, count, on_row, ctx, type, result, err);
}
