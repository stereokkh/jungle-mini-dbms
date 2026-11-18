#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_DEPTH 32   // 객체·배열 중첩 한도. [[[[...]]]] 로 스택을 터뜨리는 공격을 막는다

// ======================= 쓰기 =======================

void jb_add(JsonBuf *b, const char *s, size_t n)
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

void jb_str(JsonBuf *b, const char *s)
{
    jb_add(b, s, strlen(s));
}

void jb_int(JsonBuf *b, long long v)
{
    char tmp[32];
    int n = snprintf(tmp, sizeof tmp, "%lld", v);
    jb_add(b, tmp, (size_t)n);
}

void jb_json_str(JsonBuf *b, const char *s)
{
    jb_add(b, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            char esc[2] = { '\\', (char)*p };
            jb_add(b, esc, 2);
        } else if (*p == '\n') {
            jb_add(b, "\\n", 2);
        } else if (*p < 0x20) {
            char esc[8];
            snprintf(esc, sizeof esc, "\\u%04x", *p);
            jb_add(b, esc, 6);
        } else {
            jb_add(b, (const char *)p, 1);   // 한글(UTF-8) 등은 그대로
        }
    }
    jb_add(b, "\"", 1);
}

char *jb_finish(JsonBuf *b)
{
    char *out = b->data;
    if (b->failed) {
        free(out);
        out = NULL;
    }
    memset(b, 0, sizeof *b);
    return out;
}

// ======================= 읽기 =======================

void jr_skip_ws(JsonReader *r)
{
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r')) {
        r->p++;
    }
}

int jr_eat(JsonReader *r, char c)
{
    jr_skip_ws(r);
    if (r->p < r->end && *r->p == c) {
        r->p++;
        return 1;
    }
    return 0;
}

// [역할] 16진수 4글자(\uXXXX 의 XXXX)를 숫자로. 형식이 틀리면 -1.
static long read_hex4(const char *s)
{
    long v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else return -1;
    }
    return v;
}

// [역할] 유니코드 코드 포인트 하나를 UTF-8 바이트(1~4개)로 붙인다.
static void add_utf8(JsonBuf *b, long cp)
{
    char u[4];
    if (cp < 0x80) {
        u[0] = (char)cp;
        jb_add(b, u, 1);
    } else if (cp < 0x800) {
        u[0] = (char)(0xC0 | (cp >> 6));
        u[1] = (char)(0x80 | (cp & 0x3F));
        jb_add(b, u, 2);
    } else if (cp < 0x10000) {
        u[0] = (char)(0xE0 | (cp >> 12));
        u[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[2] = (char)(0x80 | (cp & 0x3F));
        jb_add(b, u, 3);
    } else {
        u[0] = (char)(0xF0 | (cp >> 18));
        u[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        u[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        u[3] = (char)(0x80 | (cp & 0x3F));
        jb_add(b, u, 4);
    }
}

int jr_read_string(JsonReader *r, char **out)
{
    JsonBuf b = {0};

    if (!jr_eat(r, '"')) return -1;
    jb_add(&b, "", 0);   // 빈 문자열이어도 data 가 NULL 이 아니게

    while (r->p < r->end && *r->p != '"') {
        unsigned char c = (unsigned char)*r->p++;
        if (c < 0x20) goto fail;            // 줄바꿈 같은 제어 문자는 이스케이프해야 한다
        if (c != '\\') {
            jb_add(&b, (const char *)&c, 1);
            continue;
        }
        if (r->p >= r->end) goto fail;
        char e = *r->p++;
        switch (e) {
        case '"':  jb_add(&b, "\"", 1); break;
        case '\\': jb_add(&b, "\\", 1); break;
        case '/':  jb_add(&b, "/", 1);  break;
        case 'b':  jb_add(&b, "\b", 1); break;
        case 'f':  jb_add(&b, "\f", 1); break;
        case 'n':  jb_add(&b, "\n", 1); break;
        case 'r':  jb_add(&b, "\r", 1); break;
        case 't':  jb_add(&b, "\t", 1); break;
        case 'u': {
            if (r->end - r->p < 4) goto fail;
            long cp = read_hex4(r->p);
            r->p += 4;
            if (cp < 0) goto fail;
            // 이모지 같은 큰 문자는 😀 처럼 두 개(서로게이트 쌍)로 온다 → 하나로 합친다
            if (cp >= 0xD800 && cp <= 0xDBFF) {
                if (r->end - r->p < 6 || r->p[0] != '\\' || r->p[1] != 'u') goto fail;
                long lo = read_hex4(r->p + 2);
                if (lo < 0xDC00 || lo > 0xDFFF) goto fail;
                r->p += 6;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                goto fail;                  // 짝 없는 뒷 조각
            }
            if (cp == 0) goto fail;         // \u0000 은 C 문자열을 중간에 끊으므로 받지 않는다
            add_utf8(&b, cp);
            break;
        }
        default:
            goto fail;
        }
    }
    if (r->p >= r->end) goto fail;          // 닫는 " 가 없다
    r->p++;

    *out = jb_finish(&b);
    return *out ? 0 : -1;

fail:
    free(jb_finish(&b));
    return -1;
}

// [역할] 숫자 하나를 건너뛴다: -? 정수부 (.소수부)? (e[+-]?지수)?
static int skip_number(JsonReader *r)
{
    const char *start = r->p;
    if (r->p < r->end && *r->p == '-') r->p++;
    const char *digits = r->p;
    while (r->p < r->end && *r->p >= '0' && *r->p <= '9') r->p++;
    if (r->p == digits) return -1;
    if (r->p < r->end && *r->p == '.') {
        r->p++;
        const char *frac = r->p;
        while (r->p < r->end && *r->p >= '0' && *r->p <= '9') r->p++;
        if (r->p == frac) return -1;
    }
    if (r->p < r->end && (*r->p == 'e' || *r->p == 'E')) {
        r->p++;
        if (r->p < r->end && (*r->p == '+' || *r->p == '-')) r->p++;
        const char *exp = r->p;
        while (r->p < r->end && *r->p >= '0' && *r->p <= '9') r->p++;
        if (r->p == exp) return -1;
    }
    return r->p > start ? 0 : -1;
}

// [역할] true / false / null 같은 낱말을 건너뛴다.
static int skip_word(JsonReader *r, const char *word)
{
    size_t n = strlen(word);
    if ((size_t)(r->end - r->p) < n || memcmp(r->p, word, n) != 0) return -1;
    r->p += n;
    return 0;
}

// [역할] jr_skip_value 의 본체. depth = 지금까지 들어간 중첩 단계.
static int skip_value(JsonReader *r, int depth)
{
    if (depth > MAX_DEPTH) return -1;
    jr_skip_ws(r);
    if (r->p >= r->end) return -1;

    switch (*r->p) {
    case '"': {
        char *s;
        if (jr_read_string(r, &s) != 0) return -1;
        free(s);
        return 0;
    }
    case '{':
        r->p++;
        if (jr_eat(r, '}')) return 0;               // 빈 객체
        do {
            char *key;
            jr_skip_ws(r);
            if (jr_read_string(r, &key) != 0) return -1;
            free(key);
            if (!jr_eat(r, ':')) return -1;
            if (skip_value(r, depth + 1) != 0) return -1;
        } while (jr_eat(r, ','));
        return jr_eat(r, '}') ? 0 : -1;
    case '[':
        r->p++;
        if (jr_eat(r, ']')) return 0;               // 빈 배열
        do {
            if (skip_value(r, depth + 1) != 0) return -1;
        } while (jr_eat(r, ','));
        return jr_eat(r, ']') ? 0 : -1;
    case 't': return skip_word(r, "true");
    case 'f': return skip_word(r, "false");
    case 'n': return skip_word(r, "null");
    default:  return skip_number(r);
    }
}

int jr_skip_value(JsonReader *r)
{
    return skip_value(r, 0);
}
