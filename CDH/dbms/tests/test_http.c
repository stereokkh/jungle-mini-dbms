// HTTP 파서 단위 테스트. 소켓 없이 문자열만 넣어 http_parse 를 확인한다.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "http.h"

static HttpRequest req;

// [역할] 문자열 하나를 http_parse 에 넣은 결과.
static int parse(const char *raw)
{
    return http_parse(raw, strlen(raw), &req);
}

// 정상 GET: 메서드·경로를 읽는다
static void test_get(void)
{
    assert(parse("GET /health HTTP/1.1\r\nHost: x\r\n\r\n") == 0);
    assert(strcmp(req.method, "GET") == 0);
    assert(strcmp(req.path, "/health") == 0);
    assert(req.content_length == 0);
}

// 본문은 Content-Length 만큼 다 와야 완성이다
static void test_body(void)
{
    assert(parse("POST /query HTTP/1.1\r\nContent-Length: 5\r\n\r\nhel") == 1);   // 덜 옴
    assert(parse("POST /query HTTP/1.1\r\ncontent-length: 5\r\n\r\nhello") == 0); // 대소문자 무시
    assert(req.content_length == 5);
    assert(strncmp(req.body, "hello", 5) == 0);
}

// 빈 줄이 오기 전에는 "더 받아라"
static void test_incomplete(void)
{
    assert(parse("") == 1);
    assert(parse("GET /health HTTP/1.1\r\n") == 1);
}

// 형식이 틀리면 400
static void test_bad_request(void)
{
    assert(parse("HELLO\r\n\r\n") == 400);
    assert(parse("GET health HTTP/1.1\r\n\r\n") == 400);     // 경로가 / 로 시작 안 함
    assert(parse("GET / FTP/1.1\r\n\r\n") == 400);
    assert(parse("POST / HTTP/1.1\r\nContent-Length: abc\r\n\r\n") == 400);
    assert(parse("POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n") == 400);
}

// 너무 크면 본문을 받기 전에 413
static void test_too_large(void)
{
    assert(parse("POST / HTTP/1.1\r\nContent-Length: 99999999999999\r\n\r\n") == 413);
    assert(parse("POST / HTTP/1.1\r\nContent-Length: 70000\r\n\r\n") == 413);
}

int main(void)
{
    test_get();
    test_body();
    test_incomplete();
    test_bad_request();
    test_too_large();
    printf("test_http: ok\n");
    return 0;
}
