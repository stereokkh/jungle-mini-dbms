#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "json.h"

static void test_escape(void)
{
    Buf b;
    buf_init(&b);
    json_put_string(&b, "O\"Brien\\ 김\n");
    assert(!b.failed);
    assert(strcmp(b.data, "\"O\\\"Brien\\\\ 김\\n\"") == 0);
    buf_free(&b);
    buf_free(&b);   // 두 번 불러도 안전하다
}

static void test_rows(void)
{
    Value r1[3] = { { .type = VAL_INT, .i = 1 }, { .type = VAL_STR, .s = "Kim" }, { .type = VAL_INT, .i = 20 } };
    Value r2[3] = { { .type = VAL_INT, .i = 2 }, { .type = VAL_STR, .s = "Lee" }, { .type = VAL_INT, .i = 31 } };
    Buf b;

    buf_init(&b);
    json_put_row(&b, r1, 3);
    buf_append(&b, ",");
    json_put_row(&b, r2, 3);
    assert(strcmp(b.data, "[1,\"Kim\",20],[2,\"Lee\",31]") == 0);
    buf_free(&b);
}

// 한도를 넘기면 죽지 않고 failed 만 선다. 그 뒤의 쓰기는 무시된다.
static void test_limit(void)
{
    static char big[1024 * 1024 + 1];
    Buf b;

    memset(big, 'a', sizeof big - 1);
    buf_init(&b);
    for (int i = 0; i < 65 && !b.failed; i++) {
        buf_append(&b, big);
    }
    assert(b.failed);
    assert(b.len <= MAX_RESPONSE_LEN);
    size_t len = b.len;
    buf_append(&b, "x");
    assert(b.len == len);
    buf_free(&b);
}

int main(void)
{
    test_escape();
    test_rows();
    test_limit();
    printf("test_json: ok\n");
    return 0;
}
