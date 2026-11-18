#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>

#define MAX_WORKERS 16   // 워커 수 상한
#define QUEUE_CAP   64   // accept 했지만 아직 워커가 못 집어 간 연결 수 상한

// 워커가 연결 하나를 받아서 하는 일. fd 를 닫는 것까지 이 함수의 책임이다.
typedef void (*JobFn)(int client_fd, void *ctx);

// 생산자-소비자 구조:
//   accept 스레드(생산자) ── submit ──▶ [ fd 원형 큐 ] ──▶ 워커 N개(소비자)
// 큐와 그 상태는 여러 스레드가 함께 만지므로 모두 lock 으로 지킨다.
typedef struct {
    pthread_t       threads[MAX_WORKERS];
    int             thread_count;

    int             queue[QUEUE_CAP];   // 기다리는 연결의 fd. 원형 큐
    int             head;               // 다음에 꺼낼 자리
    int             count;              // 들어 있는 개수 (넣을 자리 = (head + count) % QUEUE_CAP)

    pthread_mutex_t lock;               // queue, head, count, stopping 을 지킨다
    pthread_cond_t  not_empty;          // "큐에 일이 들어왔다 / 멈춰라" 신호
    int             stopping;

    JobFn           fn;
    void           *ctx;
} ThreadPool;

// 락과 조건 변수를 만들고 워커 thread_count 개를 띄운다. 성공 0, 실패 -1.
int threadpool_start(ThreadPool *p, int thread_count, JobFn fn, void *ctx);

// fd 를 큐에 넣고 잠든 워커 하나를 깨운다.
// 큐가 꽉 찼으면 넣지 않고 -1 (부른 쪽이 503 을 보내고 닫는다).
int threadpool_submit(ThreadPool *p, int client_fd);

// 모든 워커를 깨워 끝내고 기다린다(join). 큐에 남은 fd 는 닫는다.
void threadpool_stop(ThreadPool *p);

#endif
