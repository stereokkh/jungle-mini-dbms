#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

// 아주 작은 HTTP/1.1 처리기: 요청 하나 읽기, 파싱, 응답 보내기.
//
//   POST /query HTTP/1.1\r\n          ← 요청 줄
//   Content-Length: 21\r\n            ← 헤더
//   \r\n                              ← 빈 줄 = 헤더 끝
//   SELECT * FROM users;              ← 본문 (Content-Length 바이트)

#define HTTP_MAX_REQUEST (64 * 1024)   // 요청 전체(헤더+본문) 상한. 넘으면 413

typedef struct {
    char        method[16];      // "GET", "POST" ...
    char        path[256];       // "/health"
    size_t      content_length;  // 본문 길이 (없으면 0)
    const char *body;            // 읽기 버퍼 안을 가리킨다 (끝에 '\0' 이 붙어 있다)
} HttpRequest;

// [역할] buf[0..len) 을 요청으로 해석한다. 소켓을 모르므로 문자열만으로 테스트할 수 있다.
// [반환] 0   = 헤더+본문이 다 왔다 (req 완성)
//        1   = 아직 덜 왔다 → 더 받고 다시 부른다
//        4xx = 잘못된 요청 → 그 상태 코드로 응답한다
int http_parse(const char *buf, size_t len, HttpRequest *req);

// [역할] 소켓 fd 에서 요청 하나를 끝까지 받는다. buf 는 HTTP_MAX_REQUEST + 1 바이트.
// [반환] 0 = 성공, 4xx = 잘못된 요청(408 은 시간 초과), -1 = 아무것도 못 받고 끊김
int http_read_request(int fd, char *buf, HttpRequest *req);

// [역할] 상태 코드 + JSON 본문으로 응답을 보낸다. 응답 후 연결은 닫는다(Connection: close).
void http_send_json(int fd, int status, const char *json);

// [역할] 오류 응답 {"ok":false,"error":"..."} 를 보낸다.
void http_send_error(int fd, int status, const char *message);

// [역할] 200 → "OK" 처럼 상태 코드의 표준 문구.
const char *http_reason(int status);

#endif
