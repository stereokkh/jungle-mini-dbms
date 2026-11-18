#ifndef JSON_H
#define JSON_H

#include <stddef.h>

#include "statement.h"

// 응답 본문 한도. 100만 행 SELECT * 가 서버 메모리를 다 먹지 못하게 막는다.
#define MAX_RESPONSE_LEN (64u * 1024u * 1024u)

// 응답 본문을 쌓는 버퍼. 행 수를 미리 알 수 없어서 malloc 으로 늘린다.
typedef struct {
    char  *data;     // 항상 '\0' 으로 끝난다. 아직 아무것도 안 넣었으면 NULL
    size_t len;
    size_t cap;
    int    failed;   // 한 번이라도 늘리기에 실패했으면 1. 그 뒤의 쓰기는 무시한다
} Buf;

// 쓰기 함수가 값을 돌려주지 않는 이유: 호출마다 검사하면 코드가 검사로 덮인다.
// 다 쓴 뒤 failed 를 한 번만 본다.
void buf_init(Buf *b);
void buf_append(Buf *b, const char *s);
void buf_append_n(Buf *b, const char *s, size_t n);
void buf_free(Buf *b);   // 두 번 불러도 안전하다

void json_put_string(Buf *b, const char *s);                          // "..." 로 감싸고 이스케이프
void json_put_value(Buf *b, const Value *v);                          // INT 는 숫자, TEXT 는 문자열
void json_put_row(Buf *b, const Value *fields, int field_count);      // [1,"Kim",20]

#endif
