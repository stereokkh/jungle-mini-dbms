#ifndef JSON_H
#define JSON_H

#include <stddef.h>

// 아주 작은 JSON 도구. 외부 라이브러리 없이 우리가 필요한 만큼만 만든다.
//   쓰기: JsonBuf 에 문자열·정수·JSON 문자열("...")을 이어 붙인다
//   읽기: JsonReader 로 JSON 텍스트를 앞에서부터 훑는다 (문자열 읽기, 값 건너뛰기)

// ---------- 쓰기 ----------

// 필요한 만큼 자라는 문자열 버퍼. data 는 항상 '\0' 으로 끝난다.
typedef struct {
    char  *data;
    size_t len;      // 쓴 글자 수 ('\0' 제외)
    size_t cap;      // 할당한 크기
    int    failed;   // 메모리 할당에 한 번이라도 실패했으면 1 (이후 쓰기는 무시)
} JsonBuf;

// [역할] n 바이트를 끝에 붙인다. 자리가 모자라면 realloc 으로 두 배씩 늘린다.
void jb_add(JsonBuf *b, const char *s, size_t n);

// [역할] C 문자열 하나를 그대로 붙인다 (이스케이프 없음. JSON 기호를 쓸 때).
void jb_str(JsonBuf *b, const char *s);

// [역할] 정수 하나를 글자로 바꿔 붙인다.
void jb_int(JsonBuf *b, long long v);

// [역할] s 를 JSON 문자열("...")로 붙인다. " \ 와 제어 문자를 이스케이프한다.
void jb_json_str(JsonBuf *b, const char *s);

// [역할] 버퍼를 비운다. 메모리 실패가 있었으면 data 를 버리고 NULL 을 돌려주고,
//        아니면 data 를 돌려준다 (부른 쪽이 free).
char *jb_finish(JsonBuf *b);

// ---------- 읽기 ----------

// JSON 텍스트 [p, end) 를 앞에서부터 읽는 커서.
typedef struct {
    const char *p;
    const char *end;
} JsonReader;

// [역할] 공백(스페이스, 탭, 줄바꿈)을 건너뛴다.
void jr_skip_ws(JsonReader *r);

// [역할] 지금 위치가 c 이면 한 글자 넘기고 1, 아니면 0. (앞 공백은 건너뛴다)
int jr_eat(JsonReader *r, char c);

// [역할] 지금 위치의 JSON 문자열 "..." 을 읽어 이스케이프를 풀고, malloc 한 C 문자열로 돌려준다.
// [기능] \" \\ \/ \b \f \n \r \t \uXXXX(한글·이모지 포함 → UTF-8) 를 처리한다.
// [반환] 성공 0 (*out 은 부른 쪽이 free), 형식 오류 -1
int jr_read_string(JsonReader *r, char **out);

// [역할] 지금 위치의 JSON 값 하나(객체·배열·문자열·숫자·true/false/null)를 통째로 건너뛴다.
// [기능] 건너뛰면서 문법도 검사한다. 너무 깊게 중첩되면(32 단계) 오류로 본다.
// [반환] 성공 0, 형식 오류 -1
int jr_skip_value(JsonReader *r);

#endif
