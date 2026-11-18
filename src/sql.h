#ifndef SQL_H
#define SQL_H

#include "executor.h"

// CLI 에서 이미 나눈 토큰과 서버에서 받은 SQL 문자열은 같은 실행 경로를 쓴다.
DbStatus execute_tokens(Db *db, const Token *tokens, int count, RowCallback on_row, void *ctx,
                       StmtType *type, ExecResult *result, DbError *err);

// 비어 있거나 두 문장 이상인 요청은 실행 전에 거부한다.
DbStatus run_one_sql(Db *db, const char *sql, RowCallback on_row, void *ctx,
                     StmtType *type, ExecResult *result, DbError *err);

#endif
