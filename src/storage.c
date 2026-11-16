#include "storage.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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

DbStatus storage_append(Table *t, const char *line, long *offset, DbError *err)
{
    // 줄 끝에 붙일 줄바꿈까지 세어서 한도와 비교한다.
    if (strlen(line) + 1 > MAX_LINE_LEN) {
        return fail(err, DB_ERR_TOO_LONG, "row is too long (limit is %d bytes)", MAX_LINE_LEN);
    }
    // "ab" 로 연 파일은 쓰기 전까지 위치가 0 으로 나올 수 있어서, 끝으로 옮긴 뒤에 위치를 묻는다.
    if (fseek(t->fp, 0, SEEK_END) != 0) {
        return fail(err, DB_ERR_IO, "cannot write to table '%s'", t->name);
    }
    long start = ftell(t->fp);
    if (start < 0 || fputs(line, t->fp) == EOF || fputc('\n', t->fp) == EOF) {
        return fail(err, DB_ERR_IO, "cannot write to table '%s'", t->name);
    }
    *offset = start;
    return DB_OK;
}

// 큰따옴표로 감싼 TEXT 필드 하나를 읽는다. "" 는 " 하나다. 모양이 틀리면 0 이 아닌 값을 돌려준다.
static int read_text(const char **pp, Value *v)
{
    const char *p = *pp + 1;    // 여는 따옴표 다음부터
    size_t len = 0;

    for (;;) {
        if (*p == '\0') {
            return -1;          // 닫는 따옴표 없이 줄이 끝남
        }
        if (*p == '"') {
            if (p[1] != '"') {
                break;          // 닫는 따옴표
            }
            p++;                // "" 의 첫 번째는 건너뛰고 두 번째를 글자로 담는다
        }
        if (len >= sizeof v->s - 1) {
            return -1;
        }
        v->s[len++] = *p++;
    }
    v->s[len] = '\0';
    v->type = VAL_STR;
    *pp = p + 1;
    return 0;
}

// 따옴표 없는 INT 필드 하나를 읽는다. 모양이 틀리면 0 이 아닌 값을 돌려준다.
static int read_int(const char **pp, Value *v)
{
    char *end;

    // strtoll 은 앞의 공백과 + 도 받아 주므로, 우리가 쓰는 모양(숫자 또는 -)만 통과시킨다.
    if (!isdigit((unsigned char)**pp) && **pp != '-') {
        return -1;
    }
    errno = 0;
    v->i = strtoll(*pp, &end, 10);
    if (end == *pp || errno == ERANGE) {
        return -1;
    }
    v->type = VAL_INT;
    *pp = end;
    return 0;
}

// CSV 한 줄을 필드로 나눈다. 필드 수를 돌려주고, 모양이 틀리면 -1 을 돌려준다.
// TEXT 는 항상 큰따옴표로 감싸서 저장하므로 첫 글자만 보고 타입을 알 수 있다.
static int split_line(const char *line, Value *fields, int max)
{
    const char *p = line;
    int n = 0;

    for (;;) {
        if (n >= max) {
            return -1;
        }
        int bad = (*p == '"') ? read_text(&p, &fields[n]) : read_int(&p, &fields[n]);
        if (bad) {
            return -1;
        }
        n++;
        if (*p == '\0') {
            return n;
        }
        if (*p != ',') {
            return -1;
        }
        p++;
    }
}

// 읽은 줄이 스키마와 맞는지 본다. 여기서 걸러 두면 실행기는 타입을 믿고 비교할 수 있다.
static int matches_schema(const Schema *s, const Value *fields, int n)
{
    if (n != s->count) {
        return 0;
    }
    for (int i = 0; i < n; i++) {
        int is_int = (fields[i].type == VAL_INT);
        if (is_int != (s->cols[i].type == COL_INT)) {
            return 0;
        }
    }
    return 1;
}

// 열린 파일을 처음부터 한 줄씩 읽어 콜백을 부른다. 파일을 닫는 일은 부른 쪽이 한다.
static DbStatus scan_lines(FILE *in, const Table *t, ScanCallback cb, void *ctx, DbError *err)
{
    char line[MAX_LINE_LEN + 1];
    Value fields[16];
    int max_fields = (int)(sizeof fields / sizeof fields[0]);
    long offset = 0;

    while (fgets(line, sizeof line, in) != NULL) {
        size_t len = strlen(line);
        if (len == MAX_LINE_LEN && line[len - 1] != '\n') {
            // 버퍼가 꽉 찼는데 줄바꿈이 없으면 줄이 한도를 넘은 것이다.
            return fail(err, DB_ERR_TOO_LONG, "table '%s': row at offset %ld is too long",
                        t->name, offset);
        }
        if (len == 0 || line[len - 1] != '\n') {
            // 줄바꿈 없이 끝난 줄은 쓰다가 끊긴 것이다. 그대로 받아 주면 다음 INSERT 가
            // 이 줄 뒤에 바로 붙어서 두 행이 한 줄로 합쳐진다.
            return fail(err, DB_ERR_IO, "table '%s': incomplete row at offset %ld",
                        t->name, offset);
        }
        line[len - 1] = '\0';
        int n = split_line(line, fields, max_fields);
        if (n < 0 || !matches_schema(&t->schema, fields, n)) {
            return fail(err, DB_ERR_IO, "table '%s': corrupt row at offset %ld", t->name, offset);
        }
        if (cb(fields, n, offset, ctx) != 0) {
            return DB_OK;   // 콜백이 그만 읽으라고 했다
        }
        offset += (long)len;
    }
    if (ferror(in)) {
        return fail(err, DB_ERR_IO, "cannot read table '%s'", t->name);
    }
    return DB_OK;
}

DbStatus storage_scan(Db *db, Table *t, ScanCallback cb, void *ctx, DbError *err)
{
    char path[512];     // data_dir(255) + '/' + name(63) + ".csv" 가 항상 들어가는 크기

    // 쓰기용 버퍼에 남은 줄을 파일로 내려보내야 방금 넣은 줄이 읽힌다.
    // 테이블을 여는 중(next_id 를 정할 때)에는 아직 fp 가 없다.
    if (t->fp != NULL && fflush(t->fp) != 0) {
        return fail(err, DB_ERR_IO, "cannot write to table '%s'", t->name);
    }
    snprintf(path, sizeof path, "%s/%s.csv", db->data_dir, t->name);

    FILE *in = fopen(path, "rb");
    if (in == NULL) {
        if (errno == ENOENT) {
            return DB_OK;   // 데이터 파일이 아직 없으면 행이 0개인 것이다
        }
        return fail(err, DB_ERR_IO, "cannot open data file '%s'", path);
    }
    DbStatus st = scan_lines(in, t, cb, ctx, err);
    fclose(in);
    return st;
}
