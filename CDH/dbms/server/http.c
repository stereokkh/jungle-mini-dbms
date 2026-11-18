#include "http.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>

// [역할] 상태 코드를 응답 첫 줄에 쓸 표준 문구로 바꾼다. (200 → "OK", 404 → "Not Found")
// [반환] 모르는 코드는 "Unknown"
const char *http_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Content Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default:  return "Unknown";
    }
}

// [역할] 헤더 구간에서 Content-Length 값을 찾는다.
// [반환] 0 = 성공(없으면 *out=0), 400 = 숫자가 아님, 413 = 너무 큼
static int find_content_length(const char *headers, const char *end, size_t *out)
{
    *out = 0;
    for (const char *line = headers; line < end;) {
        const char *eol = strstr(line, "\r\n");
        if (eol == NULL || eol > end) eol = end;

        if ((size_t)(eol - line) > 15 && strncasecmp(line, "Content-Length:", 15) == 0) {
            const char *p = line + 15;
            size_t v = 0;
            while (*p == ' ') p++;
            if (p == eol) return 400;
            for (; p < eol; p++) {
                if (*p < '0' || *p > '9') return 400;
                v = v * 10 + (size_t)(*p - '0');
                if (v > HTTP_MAX_REQUEST) return 413;   // 넘치기 전에 끊는다
            }
            *out = v;
        }
        line = eol + 2;
    }
    return 0;
}

// [역할] 지금까지 받은 바이트 buf[0..len) 을 HTTP 요청으로 해석해 req 를 채운다.
// [순서] 1) 빈 줄(\r\n\r\n)을 찾아 헤더가 끝났는지 본다 (없으면 "더 받아라")
//        2) 첫 줄 "METHOD PATH HTTP/1.x" 를 sscanf 로 나눈다
//        3) 헤더에서 Content-Length 를 찾는다
//        4) 본문이 그 길이만큼 다 왔는지 본다
// [반환] 0 = 완성, 1 = 아직 덜 옴, 400/413 = 잘못된 요청
// [주의] buf 는 '\0' 으로 끝나야 한다 (strstr·sscanf 를 쓰기 때문)
int http_parse(const char *buf, size_t len, HttpRequest *req)
{
    // 1) 헤더 끝(빈 줄)이 와야 해석을 시작할 수 있다.
    const char *head_end = strstr(buf, "\r\n\r\n");
    if (head_end == NULL) {
        return len >= HTTP_MAX_REQUEST ? 413 : 1;
    }

    // 2) 요청 줄: "METHOD PATH HTTP/1.x"
    memset(req, 0, sizeof *req);
    char version[16] = "";
    if (sscanf(buf, "%15[A-Z] %255[^ \r\n] %15[^\r\n]", req->method, req->path, version) != 3 ||
        req->path[0] != '/' || strncmp(version, "HTTP/1.", 7) != 0) {
        return 400;
    }

    // 3) 본문 길이.
    int st = find_content_length(strstr(buf, "\r\n") + 2, head_end + 2, &req->content_length);
    if (st != 0) return st;

    size_t head_len = (size_t)(head_end - buf) + 4;
    if (head_len + req->content_length > HTTP_MAX_REQUEST) return 413;

    // 4) 본문이 다 왔나?
    if (len < head_len + req->content_length) return 1;
    req->body = buf + head_len;
    return 0;
}

// [역할] 소켓 fd 에서 요청 하나를 끝까지 받는다.
// [기능] recv 로 받은 바이트를 buf 에 계속 이어 붙이고, 받을 때마다 http_parse 로 "다 왔나?" 확인한다.
//        TCP 는 한 번에 다 온다는 보장이 없어서 이렇게 반복해야 한다.
// [반환] 0 = 성공(req 완성), 400/408/413 = 잘못된 요청·시간 초과·너무 큼, -1 = 아무것도 안 보내고 끊김
int http_read_request(int fd, char *buf, HttpRequest *req)
{
    size_t len = 0;

    for (;;) {
        // TCP 는 바이트 스트림이라 recv 한 번에 요청이 다 온다는 보장이 없다 → 모일 때까지 반복.
        ssize_t n = recv(fd, buf + len, HTTP_MAX_REQUEST - len, 0);
        if (n == 0) return len == 0 ? -1 : 400;          // 중간에 끊김
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)  // SO_RCVTIMEO 시간 초과
                return len == 0 ? -1 : 408;
            return -1;
        }
        len += (size_t)n;
        buf[len] = '\0';    // strstr/sscanf 를 쓰려고 항상 '\0' 으로 끝낸다

        int st = http_parse(buf, len, req);
        if (st != 1) return st;                 // 0(완성) 또는 오류 코드
        if (len >= HTTP_MAX_REQUEST) return 413;
    }
}

// [역할] len 바이트를 다 보낼 때까지 send 를 반복한다 (send 는 일부만 보낼 수 있다).
static void send_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        // MSG_NOSIGNAL: 상대가 끊었어도 SIGPIPE 로 서버가 죽지 않게 한다.
        ssize_t n = send(fd, data, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return;     // 상대가 끊었다. 이 연결만 포기한다
        data += n;
        len -= (size_t)n;
    }
}

// [역할] JSON 응답 하나를 보낸다.
// [기능] 응답 머리(상태 줄 + Content-Type + Content-Length + Connection: close + 빈 줄)를 만들어 보내고,
//        이어서 본문 json 을 보낸다. Content-Length 가 있어야 클라이언트가 본문 끝을 안다.
void http_send_json(int fd, int status, const char *json)
{
    char head[256];
    size_t body_len = strlen(json);
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     status, http_reason(status), body_len);
    send_all(fd, head, (size_t)n);
    send_all(fd, json, body_len);
}

// [역할] 오류 응답을 보낸다. 본문 모양: {"ok":false,"error":"message"}
void http_send_error(int fd, int status, const char *message)
{
    char body[512];
    // message 는 서버가 정한 고정 문구만 넣는다 (사용자 입력이 아니라 따로 이스케이프하지 않는다).
    snprintf(body, sizeof body, "{\"ok\":false,\"error\":\"%s\"}", message);
    http_send_json(fd, status, body);
}
