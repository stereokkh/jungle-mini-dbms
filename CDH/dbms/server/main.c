// mini-dbms API 서버 (2단계: JSON-RPC SQL API)
//
//   socket → bind → listen → [accept → 요청 읽기 → 라우팅 → 응답 → close] 반복
//
// 지금은 요청을 하나씩 차례로 처리하는 "반복 서버" 다.
//   GET  /health  살아 있는지 확인 (모니터링 도구용 단순 HTTP)
//   POST /rpc     JSON-RPC 2.0. method "query" 로 SQL 을 실행한다 (rpc.c)
// 3단계에서 스레드 풀로 바꾼다.

#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "db.h"
#include "http.h"
#include "rpc.h"

#define DEFAULT_PORT     8080
#define DEFAULT_DATA_DIR "data"   // 테이블 파일(.schema, .csv)이 있는 폴더
#define TIMEOUT_SEC      5        // 요청을 이 시간 안에 다 보내지 않으면 408

// 서버가 쓰는 DB 하나. main 에서 열고, 모든 요청이 함께 쓴다.
// (반복 서버라 한 번에 한 요청만 쓰므로 아직 락이 필요 없다. 3단계에서 락을 붙인다.)
static Db g_db;

// Ctrl+C(SIGINT) 를 받으면 1. 시그널 핸들러에서는 이런 플래그만 바꾸는 것이 안전하다.
static volatile sig_atomic_t g_stop = 0;

// [역할] Ctrl+C(SIGINT)·kill(SIGTERM) 을 받으면 OS 가 대신 불러 주는 함수.
// [기능] 종료 플래그 g_stop 만 1 로 바꾼다. 실제 정리는 main 의 루프가 플래그를 보고 한다.
//        (시그널 핸들러 안에서 printf·free 같은 함수를 부르면 위험하기 때문)
static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

// [역할] port 에서 손님(연결)을 기다리는 "리스닝 소켓" 을 만든다.
// [순서] socket(소켓 생성) → setsockopt(SO_REUSEADDR) → bind(포트에 묶기) → listen(대기 시작)
// [반환] 리스닝 소켓 fd. 실패하면 -1
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

// [역할] GET /health 요청을 처리한다. 서버가 살아 있는지 확인하는 용도.
// [기능] 200 과 {"ok":true,"status":"up"} 를 보낸다.
static void handle_health(int fd)
{
    http_send_json(fd, 200, "{\"ok\":true,\"status\":\"up\"}");
}

// [역할] POST /rpc 요청을 처리한다. 본문의 JSON-RPC 요청을 실행하고 JSON-RPC 응답을 보낸다.
// [순서] rpc_handle 에 본문을 넘긴다 → 응답 JSON 을 HTTP 200 으로 보낸다
// [기능] JSON-RPC 는 성공·실패를 본문의 result / error 로 구분하므로 HTTP 상태는 항상 200 이다.
//        알림(id 없는 요청)이면 보낼 응답이 없으므로 204(No Content).
//        메모리가 모자라 응답을 못 만들었을 때만 HTTP 500.
static void handle_rpc(int fd, const HttpRequest *req)
{
    int notification = 0;
    char *json = rpc_handle(&g_db, req->body, req->content_length, &notification);

    if (json != NULL) http_send_json(fd, 200, json);
    else if (notification) http_send_json(fd, 204, "");
    else http_send_error(fd, 500, "out of memory");
    free(json);
}

// [역할] 요청의 경로(path)와 메서드(GET/POST)를 보고 알맞은 처리 함수(핸들러)를 부른다.
// [기능] 경로가 없으면 404, 경로는 있는데 메서드가 틀리면 405 를 보낸다.
//        새 API 를 추가할 때는 여기에 if 하나를 더한다.
static void route(int fd, const HttpRequest *req)
{
    if (strcmp(req->path, "/health") == 0) {
        if (strcmp(req->method, "GET") == 0) handle_health(fd);
        else http_send_error(fd, 405, "method not allowed");
        return;
    }
    if (strcmp(req->path, "/rpc") == 0) {
        if (strcmp(req->method, "POST") == 0) handle_rpc(fd, req);
        else http_send_error(fd, 405, "method not allowed");
        return;
    }
    http_send_error(fd, 404, "not found");
}

// [역할] accept 로 받은 연결 하나를 처음부터 끝까지 처리한다.
// [순서] recv 시간 제한 설정 → http_read_request 로 요청 읽기
//        → 성공이면 route 로 응답, 잘못된 요청이면 오류 응답(400/408/413) → 로그 출력 → close
// [인자] fd = accept 가 돌려준 연결 소켓
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

// [역할] 서버 프로그램의 시작점. 서버를 켜고, 요청을 계속 받다가, Ctrl+C 를 받으면 끈다.
// [순서]
//   1) 인자 읽기: 포트(없으면 8080), 데이터 폴더(없으면 data)   (예: ./dbms-server 9000 ../../data)
//      → db_open 으로 DB 준비 (테이블 파일은 처음 쓸 때 연다)
//   2) 시그널 설정: SIGPIPE 는 무시, SIGINT/SIGTERM 은 on_signal 로 종료 플래그만 세움
//   3) open_listener 로 리스닝 소켓을 연다. 실패하면(포트 사용 중 등) 종료 코드 1
//   4) 메인 루프: accept 로 연결을 받고 → handle_client 로 처리 → 다시 accept (g_stop 이 설 때까지)
//   5) 리스닝 소켓과 DB 를 닫고 종료 코드 0
// [인자] argv[1] = 포트 번호 (선택), argv[2] = 데이터 폴더 (선택)
// [반환] 0 = 정상 종료, 1 = 서버 시작 실패
int main(int argc, char **argv)
{
    // 1) 인자 읽기 + DB 준비
    int port = argc > 1 ? atoi(argv[1]) : DEFAULT_PORT;
    const char *data_dir = argc > 2 ? argv[2] : DEFAULT_DATA_DIR;

    DbError err;
    if (db_open(data_dir, &g_db, &err) != DB_OK) {
        fprintf(stderr, "%s\n", err.message);
        return 1;
    }

    // 2) 시그널 설정
    // 끊긴 소켓에 쓰면 SIGPIPE 로 프로세스가 죽는다 → 무시하고 send 의 오류로 받는다.
    signal(SIGPIPE, SIG_IGN);

    // SA_RESTART 를 빼서 Ctrl+C 를 받으면 막혀 있던 accept 가 깨어나게 한다.
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // 3) 리스닝 소켓 열기
    int lfd = open_listener(port);
    if (lfd < 0) {
        perror("listen");
        return 1;
    }
    printf("listening on port %d (data: %s)\n", port, data_dir);
    fflush(stdout);

    // 4) 메인 루프: 연결 받기 → 처리 → 반복
    while (!g_stop) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) continue;   // 시그널(EINTR) 등 → 루프 조건에서 g_stop 확인
        handle_client(cfd);
    }

    // 5) 정리
    printf("shutting down\n");
    close(lfd);
    db_close(&g_db);   // 열어 둔 테이블 파일을 닫고(버퍼를 디스크로 내림) 인덱스를 해제
    return 0;
}
