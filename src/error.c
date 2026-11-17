#include "error.h"

#include <stdarg.h>
#include <stdio.h>

DbStatus error_set(DbError *err, DbStatus code, int pos, const char *fmt, ...)
{
    size_t size = sizeof err->message;
    int n;
    va_list args;

    err->code = code;
    err->pos = pos;

    // 앞머리를 먼저 쓰고, 그 뒤에 이어서 본문을 쓴다.
    if (code == DB_ERR_SYNTAX && pos >= 0) {
        n = snprintf(err->message, size, "Syntax error at position %d: ", pos);
    } else if (code == DB_ERR_SYNTAX) {
        n = snprintf(err->message, size, "Syntax error: ");
    } else {
        n = snprintf(err->message, size, "Error: ");
    }

    // 본문이 길면 vsnprintf 가 버퍼 크기에 맞춰 자른다. 넘겨 쓰지 않는다.
    va_start(args, fmt);
    vsnprintf(err->message + n, size - (size_t)n, fmt, args);
    va_end(args);
    return code;
}
