#ifndef SERVER_H
#define SERVER_H

#include "db.h"
#include "json.h"
#include "threadpool.h"

// 서버 상태는 모두 여기에 둔다 (전역 변수를 쓰지 않는다).
// accept 스레드는 연결을 큐에 넣기만 하고, 워커들이 꺼내서 처리한다.
typedef struct {
    Db             *db;         // main 이 한 번만 열어 둔 DB. 모든 워커가 함께 쓴다
    pthread_mutex_t db_lock;    // 그래서 execute 는 한 번에 한 워커만 부른다
    int             listen_fd;  // 입구 역할만 하는 소켓
    ThreadPool      pool;
} Server;

// SQL 문장 하나를 실행해 JSON 을 out 에 쓰고, 보낼 HTTP 상태 코드를 돌려준다.
// 소켓을 건드리지 않아서 서버를 띄우지 않고도 단위 테스트할 수 있다.
int server_handle_sql(Server *s, const char *sql, Buf *out);

// 연결 하나를 처리한다: 요청 읽기 → 검사 → server_handle_sql → 응답 쓰기 → close.
// client_fd 를 닫는 것까지 이 함수의 책임이다. (2겹에서 스레드 풀의 작업 함수가 된다)
void server_handle_client(int client_fd, void *ctx);

// socket → bind → listen, 워커 thread_count 개를 띄운 뒤 accept 를 영원히 반복한다.
// 소켓을 못 열거나 워커를 못 띄우면 -1.
int server_run(Db *db, int port, int thread_count);

#endif
