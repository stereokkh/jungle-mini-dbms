// JSON 도구 단위 테스트. PR #6 의 tests/test_json.c 에서 이스케이프·행 테스트를 이 서버의 json.c 로 옮기고,
// 읽기(JsonReader) 테스트를 더했다.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

// [역할] " \ 줄바꿈은 이스케이프하고, 한글(UTF-8)은 그대로 둔다.
static void test_escape(void)
{
    JsonBuf b = {0};
    jb_json_str(&b, "O\"Brien\\ 김\n");
    char *s = jb_finish(&b);
    assert(s != NULL);
    assert(strcmp(s, "\"O\\\"Brien\\\\ 김\\n\"") == 0);
    free(s);
}

// [역할] 행 두 개를 [1,"Kim",20],[2,"Lee",31] 모양으로 이어 붙인다.
static void test_rows(void)
{
    JsonBuf b = {0};
    jb_str(&b, "[");
    jb_int(&b, 1);
    jb_str(&b, ",");
    jb_json_str(&b, "Kim");
    jb_str(&b, ",");
    jb_int(&b, 20);
    jb_str(&b, "],[");
    jb_int(&b, 2);
    jb_str(&b, ",");
    jb_json_str(&b, "Lee");
    jb_str(&b, ",");
    jb_int(&b, 31);
    jb_str(&b, "]");
    char *s = jb_finish(&b);
    assert(strcmp(s, "[1,\"Kim\",20],[2,\"Lee\",31]") == 0);
    free(s);
}

// [역할] 쓴 문자열을 다시 읽으면 원래 값이 나온다 (\uXXXX 포함).
static void test_round_trip(void)
{
    const char *text = "\"a\\\"b\\u00e9\\uac00\"";   // JSON 텍스트 "a\"b\u00e9\uac00" → a"bé가
    JsonReader r = { text, text + strlen(text) };
    char *out = NULL;
    assert(jr_read_string(&r, &out) == 0);
    assert(strcmp(out, "a\"b\xc3\xa9\xea\xb0\x80") == 0);
    free(out);

    const char *bad = "{\"sql\":";
    JsonReader r2 = { bad, bad + strlen(bad) };
    assert(jr_skip_value(&r2) == -1);   // 끝나지 않은 객체
}

int main(void)
{
    test_escape();
    test_rows();
    test_round_trip();
    printf("test_json: ok\n");
    return 0;
}
