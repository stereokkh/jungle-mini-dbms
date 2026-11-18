// mini-dbms API 서버 (1단계: 베이스 서버)
//
//   socket → bind → listen → [accept → 요청 읽기 → 라우팅 → 응답 → close] 반복
//
// 지금은 요청을 하나씩 차례로 처리하는 "반복 서버" 다.
// 2단계에서 POST /query 로 엔진을 붙이고, 3단계에서 스레드 풀로 바꾼다.

#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "http.h"

#define DEFAULT_PORT 8080
#define TIMEOUT_SEC  5       // 요청을 이 시간 안에 다 보내지 않으면 408

// Ctrl+C(SIGINT) 를 받으면 1. 시그널 핸들러에서는 이런 플래그만 바꾸는 것이 안전하다.
static volatile sig_atomic_t g_stop = 0;

// [역할] SIGINT/SIGTERM 핸들러: 종료 플래그만 세운다.
static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

// [역할] port 에서 연결을 기다리는 리스닝 소켓을 만든다. 실패하면 -1.
static int open_listener(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    // 서버를 껐다 바로 켜도 "Address already in use" 가 나지 않게 한다.
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   // 모든 네트워크 인터페이스에서 받는다
    addr.sin_port = htons((unsigned short)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(fd, 128) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// [역할] GET /health — 서버가 살아 있는지 확인.
static void handle_health(int fd)
{
    http_send_json(fd, 200, "{\"ok\":true,\"status\":\"up\"}");
}

// [역할] 경로·메서드를 보고 알맞은 핸들러를 부른다. 없으면 404, 메서드가 틀리면 405.
static void route(int fd, const HttpRequest *req)
{
    if (strcmp(req->path, "/health") == 0) {
        if (strcmp(req->method, "GET") == 0) handle_health(fd);
        else http_send_error(fd, 405, "method not allowed");
        return;
    }
    http_send_error(fd, 404, "not found");
}

// [역할] 연결 하나를 처리하고 닫는다: 요청 읽기 → 라우팅 → 응답.
static void handle_client(int fd)
{
    static char buf[HTTP_MAX_REQUEST + 1];   // 반복 서버라 버퍼 하나를 돌려 쓴다
    HttpRequest req;

    // 느린 클라이언트가 서버를 영원히 붙잡지 못하게 recv 에 시간 제한을 건다.
    struct timeval tv = { TIMEOUT_SEC, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    int st = http_read_request(fd, buf, &req);
    if (st == 0) {
        route(fd, &req);
        printf("%s %s\n", req.method, req.path);
    } else if (st > 0) {
        http_send_error(fd, st, http_reason(st));
        printf("bad request -> %d\n", st);
    }
    fflush(stdout);
    close(fd);   // 닫지 않으면 fd 가 새서 언젠가 accept 가 실패한다
}

int main(int argc, char **argv)
{
    int port = argc > 1 ? atoi(argv[1]) : DEFAULT_PORT;

    // 끊긴 소켓에 쓰면 SIGPIPE 로 프로세스가 죽는다 → 무시하고 send 의 오류로 받는다.
    signal(SIGPIPE, SIG_IGN);

    // SA_RESTART 를 빼서 Ctrl+C 를 받으면 막혀 있던 accept 가 깨어나게 한다.
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int lfd = open_listener(port);
    if (lfd < 0) {
        perror("listen");
        return 1;
    }
    printf("listening on port %d\n", port);
    fflush(stdout);

    while (!g_stop) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) continue;   // 시그널(EINTR) 등 → 루프 조건에서 g_stop 확인
        handle_client(cfd);
    }

    printf("shutting down\n");
    close(lfd);
    return 0;
}
