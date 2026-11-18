#include "storage.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

DbStatus storage_append(Table *t, const char *line, long *offset, DbError *err)
{
    // 줄 끝에 붙일 줄바꿈까지 세어서 한도와 비교한다.
    if (strlen(line) + 1 > MAX_LINE_LEN) {
        return error_set(err, DB_ERR_TOO_LONG, -1, "row is too long (limit is %d bytes)", MAX_LINE_LEN);
    }
    // "ab" 로 연 파일은 쓰기 전까지 위치가 0 으로 나올 수 있어서, 끝으로 옮긴 뒤에 위치를 묻는다.
    if (fseek(t->fp, 0, SEEK_END) != 0) {
        return error_set(err, DB_ERR_IO, -1, "cannot write to table '%s'", t->name);
    }
    long start = ftell(t->fp);
    if (start < 0) {
        return error_set(err, DB_ERR_IO, -1, "cannot write to table '%s'", t->name);
    }
    // 쓰기 락을 보유한 동안 버퍼를 비워 읽기 경로는 쓰기 스트림에 접근하지 않게 한다.
    if (fputs(line, t->fp) == EOF || fputc('\n', t->fp) == EOF || fflush(t->fp) == EOF) {
        t->write_failed = true;
        return error_set(err, DB_ERR_IO, -1, "cannot write to table '%s'", t->name);
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

// 파일의 지금 위치에서 줄 하나를 읽어 fields 로 나눈다. offset 은 오류 메시지에만 쓴다.
// 읽었으면 *len 에 줄바꿈까지 센 길이를, 파일 끝이라 읽을 줄이 없으면 0 을 넣는다.
static DbStatus read_row(FILE *in, const Table *t, long offset, Value *fields, int max_fields,
                         int *field_count, size_t *len, DbError *err)
{
    char line[MAX_LINE_LEN + 1];

    *len = 0;
    if (fgets(line, sizeof line, in) == NULL) {
        if (ferror(in)) {
            return error_set(err, DB_ERR_IO, -1, "cannot read table '%s'", t->name);
        }
        return DB_OK;   // 파일 끝
    }
    size_t n = strlen(line);
    if (n == MAX_LINE_LEN && line[n - 1] != '\n') {
        // 버퍼가 꽉 찼는데 줄바꿈이 없으면 줄이 한도를 넘은 것이다.
        return error_set(err, DB_ERR_TOO_LONG, -1, "table '%s': row at offset %ld is too long",
                         t->name, offset);
    }
    if (n == 0 || line[n - 1] != '\n') {
        // 줄바꿈 없이 끝난 줄은 쓰다가 끊긴 것이다. 그대로 받아 주면 다음 INSERT 가
        // 이 줄 뒤에 바로 붙어서 두 행이 한 줄로 합쳐진다.
        return error_set(err, DB_ERR_IO, -1, "table '%s': incomplete row at offset %ld",
                         t->name, offset);
    }
    line[n - 1] = '\0';
    *field_count = split_line(line, fields, max_fields);
    if (*field_count < 0 || !matches_schema(&t->schema, fields, *field_count)) {
        return error_set(err, DB_ERR_IO, -1, "table '%s': corrupt row at offset %ld", t->name, offset);
    }
    *len = n;
    return DB_OK;
}

// 열린 파일을 처음부터 한 줄씩 읽어 콜백을 부른다. 파일을 닫는 일은 부른 쪽이 한다.
static DbStatus scan_lines(FILE *in, const Table *t, ScanCallback cb, void *ctx, DbError *err)
{
    Value fields[16];
    int max_fields = (int)(sizeof fields / sizeof fields[0]);
    long offset = 0;

    for (;;) {
        int n;
        size_t len;
        DbStatus st = read_row(in, t, offset, fields, max_fields, &n, &len, err);
        if (st != DB_OK) {
            return st;
        }
        if (len == 0) {
            return DB_OK;   // 파일 끝
        }
        if (cb(fields, n, offset, ctx) != 0) {
            return DB_OK;   // 콜백이 그만 읽으라고 했다
        }
        offset += (long)len;
    }
}

// SELECT 마다 데이터 파일을 읽기용으로 따로 연다.
// 데이터 파일이 아직 없으면 *in 을 NULL 로 두고 DB_OK 를 돌려준다 (행이 0개인 것이다).
static DbStatus open_for_read(Db *db, Table *t, FILE **in, DbError *err)
{
    char path[512];     // data_dir(255) + '/' + name(63) + ".csv" 가 항상 들어가는 크기

    snprintf(path, sizeof path, "%s/%s.csv", db->data_dir, t->name);

    *in = fopen(path, "rb");
    if (*in == NULL && errno != ENOENT) {
        return error_set(err, DB_ERR_IO, -1, "cannot open data file '%s'", path);
    }
    return DB_OK;
}

DbStatus storage_scan(Db *db, Table *t, ScanCallback cb, void *ctx, DbError *err)
{
    FILE *in;
    DbStatus st = open_for_read(db, t, &in, err);

    if (st != DB_OK || in == NULL) {
        return st;
    }
    st = scan_lines(in, t, cb, ctx, err);
    fclose(in);
    return st;
}

DbStatus storage_read_at(Db *db, Table *t, long offset, ScanCallback cb, void *ctx, DbError *err)
{
    Value fields[16];
    int n;
    size_t len;
    FILE *in;
    DbStatus st = open_for_read(db, t, &in, err);

    if (st != DB_OK) {
        return st;
    }
    // 인덱스가 가리킨 줄인데 파일이 없거나 그 자리에 줄이 없으면 인덱스와 파일이 어긋난 것이다.
    if (in == NULL) {
        return error_set(err, DB_ERR_IO, -1, "table '%s': no row at offset %ld", t->name, offset);
    }
    if (fseek(in, offset, SEEK_SET) != 0) {
        fclose(in);
        return error_set(err, DB_ERR_IO, -1, "cannot read table '%s'", t->name);
    }
    st = read_row(in, t, offset, fields, (int)(sizeof fields / sizeof fields[0]), &n, &len, err);
    fclose(in);
    if (st != DB_OK) {
        return st;
    }
    if (len == 0) {
        return error_set(err, DB_ERR_IO, -1, "table '%s': no row at offset %ld", t->name, offset);
    }
    cb(fields, n, offset, ctx);
    return DB_OK;
}
