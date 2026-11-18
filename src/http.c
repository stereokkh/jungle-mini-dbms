#include "http.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>

// recv 를 한 번 부른다. 신호 때문에 끊긴 경우(EINTR)만 다시 부른다.
// 받은 바이트 수, 상대가 닫았으면 0, 오류나 제한 시간 초과면 -1.
static ssize_t recv_some(int fd, char *buf, size_t cap)
{
    for (;;) {
        ssize_t n = recv(fd, buf, cap, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return n;
    }
}

// 첫 줄 "POST /query HTTP/1.1" 에서 메서드와 경로를 꺼낸다. 모양이 틀리면 -1.
static int parse_request_line(const char *head, HttpRequest *req)
{
    char version[16];
    // %7s, %63s: 배열 크기 - 1 까지만 읽어서 넘치지 않게 한다
    if (sscanf(head, "%7s %63s %15s", req->method, req->path, version) != 3) {
        return -1;
    }
    if (strncmp(version, "HTTP/", 5) != 0) {
        return -1;
    }
    return 0;
}

// 헤더에서 Content-Length 를 찾는다. 없으면 0.
// 숫자가 아니거나 음수면 -1, MAX_BODY_LEN 을 넘으면 -2.
static long parse_content_length(const char *head, const char *head_end)
{
    const char *line = strstr(head, "\r\n");   // 첫 줄(요청 줄)은 건너뛴다
    while (line != NULL && line < head_end) {
        line += 2;
        if (strncasecmp(line, "Content-Length:", 15) == 0) {   // 헤더 이름은 대소문자를 가리지 않는다
            const char *p = line + 15;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
            char *end;
            errno = 0;
            long n = strtol(p, &end, 10);
            // 숫자 뒤에는 줄 끝(\r, 마지막 헤더면 \0)이나 공백만 와야 한다
            if (end == p || n < 0 || errno != 0 || (*end != '\r' && *end != '\0' && *end != ' ')) {
                return -1;
            }
            return (n > MAX_BODY_LEN) ? -2 : n;
        }
        line = strstr(line, "\r\n");
    }
    return 0;
}

int http_read_request(int fd, HttpRequest *req)
{
    // 헤더를 받는 버퍼. 한 번의 recv 에 본문 앞부분이 딸려 올 수 있어서 2) 에서 옮긴다.
    char buf[MAX_HEADER_LEN + 1];
    size_t len = 0;
    char *head_end = NULL;

    memset(req, 0, sizeof *req);

    // 1) 빈 줄(\r\n\r\n)이 보일 때까지 읽는다. 한 번의 recv 로 다 온다는 보장이 없다.
    while (head_end == NULL) {
        if (len == MAX_HEADER_LEN) {
            return 400;   // 헤더가 한도를 넘었다
        }
        ssize_t n = recv_some(fd, buf + len, MAX_HEADER_LEN - len);
        if (n <= 0) {
            return 400;   // 헤더를 다 보내기 전에 끊었거나 5초 동안 아무것도 안 보냈다
        }
        len += (size_t)n;
        buf[len] = '\0';
        head_end = memmem(buf, len, "\r\n\r\n", 4);   // 바이트 단위로 찾는다 (본문에 \0 이 있어도 된다)
    }
    *head_end = '\0';   // 이제 buf 는 헤더만 담은 문자열이다

    if (parse_request_line(buf, req) != 0) {
        return 400;
    }
    long body_len = parse_content_length(buf, head_end);
    if (body_len == -1) {
        return 400;
    }
    if (body_len == -2) {
        return 413;
    }

    // 2) 헤더 뒤에 같이 딸려 온 본문 조각을 먼저 옮긴다
    char  *body_start = head_end + 4;
    size_t have = len - (size_t)(body_start - buf);
    if (have > (size_t)body_len) {
        have = (size_t)body_len;   // Content-Length 뒤의 바이트는 버린다 (keep-alive 를 안 한다)
    }
    memcpy(req->body, body_start, have);

    // 3) 나머지 본문을 Content-Length 만큼 찰 때까지 읽는다
    while (have < (size_t)body_len) {
        ssize_t n = recv_some(fd, req->body + have, (size_t)body_len - have);
        if (n <= 0) {
            return 400;   // Content-Length 보다 적게 보내고 끊었거나 멈췄다
        }
        have += (size_t)n;
    }
    req->body[have] = '\0';
    req->body_len = (int)have;
    return 0;
}

static const char *status_text(int status)
{
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 503: return "Service Unavailable";
    default:  return "Internal Server Error";
    }
}

// len 바이트를 다 보낼 때까지 send 를 반복한다. send 도 일부만 보내고 돌아올 수 있다.
// MSG_NOSIGNAL: 클라이언트가 먼저 끊었을 때 SIGPIPE 로 서버 프로세스 전체가 죽지 않게 한다.
static int send_all(int fd, const char *p, size_t len)
{
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

int http_write_response(int fd, int status, const char *body, size_t body_len)
{
    char head[256];
    // Access-Control-Allow-Origin: Mini React 페이지(다른 출처)의 fetch 가 응답을 읽을 수 있게 한다
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     status, status_text(status), body_len);
    if (send_all(fd, head, (size_t)n) != 0) {
        return -1;
    }
    return send_all(fd, body, body_len);
}
