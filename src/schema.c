#include "schema.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

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

// "이름,타입" 한 줄을 Column 으로 바꾼다. line 은 줄바꿈을 뗀 상태로 들어온다.
static DbStatus parse_column(const char *line, int line_no, Column *col, DbError *err)
{
    const char *comma = strchr(line, ',');

    if (comma == NULL || comma == line) {
        return fail(err, DB_ERR_SCHEMA, "schema line %d: expected 'name,TYPE'", line_no);
    }
    size_t len = (size_t)(comma - line);
    if (len >= sizeof col->name) {
        return fail(err, DB_ERR_TOO_LONG, "schema line %d: column name is too long (limit is %d)",
                    line_no, (int)sizeof col->name - 1);
    }
    memcpy(col->name, line, len);
    col->name[len] = '\0';

    if (strcmp(comma + 1, "INT") == 0) {
        col->type = COL_INT;
    } else if (strcmp(comma + 1, "TEXT") == 0) {
        col->type = COL_TEXT;
    } else {
        return fail(err, DB_ERR_SCHEMA, "schema line %d: type must be INT or TEXT", line_no);
    }
    return DB_OK;
}

// 파일을 한 줄씩 읽어 out 에 컬럼을 채운다. 파일을 닫는 일은 부른 쪽이 한다.
static DbStatus read_columns(FILE *fp, Schema *out, DbError *err)
{
    int max_cols = (int)(sizeof out->cols / sizeof out->cols[0]);
    char line[128];     // 이름 63 + ',' + 타입 + 줄바꿈이 넉넉히 들어가는 크기
    int line_no = 0;

    while (fgets(line, sizeof line, fp) != NULL) {
        line_no++;
        // 버퍼가 꽉 찼는데 줄바꿈이 없으면 줄이 잘린 것이다. 잘린 채로 해석하지 않는다.
        if (strlen(line) == sizeof line - 1 && line[sizeof line - 2] != '\n') {
            return fail(err, DB_ERR_TOO_LONG, "schema line %d is too long", line_no);
        }
        // Windows 에서 고친 파일은 줄 끝이 \r\n 이라 둘 다 뗀다.
        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0') {
            continue;   // 빈 줄 (보통 파일 끝)
        }
        if (out->count >= max_cols) {
            return fail(err, DB_ERR_TOO_LONG, "schema has too many columns (limit is %d)", max_cols);
        }
        DbStatus st = parse_column(line, line_no, &out->cols[out->count], err);
        if (st != DB_OK) {
            return st;
        }
        out->count++;
    }
    if (ferror(fp)) {
        return fail(err, DB_ERR_IO, "cannot read schema file");
    }
    return DB_OK;
}

DbStatus schema_load(const char *path, Schema *out, DbError *err)
{
    FILE *fp = fopen(path, "r");

    if (fp == NULL) {
        // 파일이 없는 것은 "그런 테이블이 없다"는 뜻이고, 그 밖의 실패는 입출력 문제다.
        DbStatus code = (errno == ENOENT) ? DB_ERR_NO_TABLE : DB_ERR_IO;
        return fail(err, code, "cannot open schema file '%s'", path);
    }
    out->count = 0;
    DbStatus st = read_columns(fp, out, err);
    fclose(fp);
    if (st != DB_OK) {
        return st;
    }

    if (out->count == 0 || strcmp(out->cols[0].name, "id") != 0 || out->cols[0].type != COL_INT) {
        return fail(err, DB_ERR_SCHEMA, "first column of schema must be 'id,INT'");
    }
    return DB_OK;
}
