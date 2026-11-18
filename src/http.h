#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

#define MAX_HEADER_LEN 4096   // 요청 줄 + 헤더 + 빈 줄
#define MAX_BODY_LEN   8192   // 본문 = SQL 문장 하나

// 서버가 쓰는 만큼만 꺼낸 요청. 헤더는 Content-Length 만 읽고 버린다.
typedef struct {
    char method[8];                // "POST"
    char path[64];                 // "/query"
    char body[MAX_BODY_LEN + 1];   // '\0' 으로 끝낸다
    int  body_len;
} HttpRequest;

// 헤더 끝(\r\n\r\n)까지 읽고, Content-Length 만큼 본문을 읽는다.
// read 는 요청을 한 번에 다 주지 않을 수 있어서 둘 다 반복해서 읽는다.
// 성공하면 0, 실패하면 클라이언트에게 보낼 상태 코드(400, 413)를 돌려준다.
int http_read_request(int fd, HttpRequest *req);

// 상태 줄 + 헤더 + 본문을 끝까지 쓴다. 다 썼으면 0, 아니면 -1.
int http_write_response(int fd, int status, const char *body, size_t body_len);

#endif
