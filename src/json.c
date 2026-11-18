#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void buf_init(Buf *b)
{
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->failed = 0;
}

void buf_free(Buf *b)
{
    free(b->data);
    buf_init(b);
}

// extra 바이트(+ '\0')를 더 넣을 자리를 만든다. 실패하면 failed 를 세우고 -1.
static int buf_reserve(Buf *b, size_t extra)
{
    if (b->failed) {
        return -1;
    }
    if (extra > MAX_RESPONSE_LEN - b->len) {
        b->failed = 1;   // 한도 초과
        return -1;
    }
    size_t need = b->len + extra + 1;
    if (need <= b->cap) {
        return 0;
    }
    size_t cap = (b->cap == 0) ? 256 : b->cap;
    while (cap < need) {
        cap *= 2;   // 두 배씩 늘려서 append 를 n 번 해도 복사 비용이 O(n) 이 되게 한다
    }
    char *p = realloc(b->data, cap);
    if (p == NULL) {
        b->failed = 1;   // 원래 data 는 그대로 남아 있어서 buf_free 로 해제된다
        return -1;
    }
    b->data = p;
    b->cap = cap;
    return 0;
}

void buf_append_n(Buf *b, const char *s, size_t n)
{
    if (buf_reserve(b, n) != 0) {
        return;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

void buf_append(Buf *b, const char *s)
{
    buf_append_n(b, s, strlen(s));
}

// JSON 문자열 규칙: " 와 \ 그리고 0x20 미만 제어 문자만 이스케이프하면 된다.
// 한글 같은 UTF-8 바이트는 0x80 이상이라 그대로 보낸다.
void json_put_string(Buf *b, const char *s)
{
    buf_append_n(b, "\"", 1);
    for (const char *p = s; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"') {
            buf_append_n(b, "\\\"", 2);
        } else if (c == '\\') {
            buf_append_n(b, "\\\\", 2);
        } else if (c == '\n') {
            buf_append_n(b, "\\n", 2);
        } else if (c == '\r') {
            buf_append_n(b, "\\r", 2);
        } else if (c == '\t') {
            buf_append_n(b, "\\t", 2);
        } else if (c < 0x20) {
            char esc[8];
            snprintf(esc, sizeof esc, "\\u%04x", c);
            buf_append_n(b, esc, 6);
        } else {
            buf_append_n(b, p, 1);
        }
    }
    buf_append_n(b, "\"", 1);
}

void json_put_value(Buf *b, const Value *v)
{
    if (v->type == VAL_INT) {
        char num[32];
        int n = snprintf(num, sizeof num, "%lld", v->i);
        buf_append_n(b, num, (size_t)n);
    } else {
        json_put_string(b, v->s);
    }
}

void json_put_row(Buf *b, const Value *fields, int field_count)
{
    buf_append_n(b, "[", 1);
    for (int i = 0; i < field_count; i++) {
        if (i > 0) {
            buf_append_n(b, ",", 1);
        }
        json_put_value(b, &fields[i]);
    }
    buf_append_n(b, "]", 1);
}
