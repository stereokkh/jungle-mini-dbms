// 스레드 풀 단위 테스트. PR #6 의 tests/test_threadpool.c 를 이 서버의 pool.c API 로 옮겼다.
//   - 작업 100개 → 정확히 100번 실행
//   - 작업자가 막혀 있으면 큐가 POOL_QUEUE_SIZE 개에서 차고, 다음 submit 은 -1 (서버는 503)
//   - 작업자 수가 범위 밖이면 시작 실패
// 진짜 연결 대신 가짜 fd(-1)를 넣는다. handler 가 fd 를 닫지 않아도 된다.

#include <assert.h>
#include <pthread.h>
#include <stdio.h>

#include "pool.h"

// 작업 함수가 함께 쓰는 카운터. pool 의 handler 는 ctx 인자가 없어서 전역에 둔다.
static pthread_mutex_t g_lock    = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_changed = PTHREAD_COND_INITIALIZER;
static int g_started;     // 시작한 작업 수
static int g_done;        // 끝난 작업 수
static int g_gate_open;   // 0 이면 작업이 문 앞에서 기다린다 (큐를 채우는 테스트용)

// [역할] 작업 하나: 시작을 알리고, 문이 열릴 때까지 기다렸다가, 끝을 센다.
static void counting_job(int fd)
{
    (void)fd;
    pthread_mutex_lock(&g_lock);
    g_started++;
    pthread_cond_broadcast(&g_changed);
    while (!g_gate_open) pthread_cond_wait(&g_changed, &g_lock);
    g_done++;
    pthread_cond_broadcast(&g_changed);
    pthread_mutex_unlock(&g_lock);
}

// [역할] 카운터를 0 으로 되돌리고 문을 열거나 닫아 둔다.
static void reset(int gate_open)
{
    g_started = g_done = 0;
    g_gate_open = gate_open;
}

// [역할] 작업 100개를 넣으면 작업자 4개가 나눠서 정확히 100번 실행한다.
static void test_runs_every_job_once(void)
{
    reset(1);
    assert(pool_start(4, counting_job) == 0);
    for (int i = 0; i < 100; i++) {
        while (pool_submit(-1) != 0) {
            // 큐가 잠깐 찼으면 작업자가 비울 때까지 다시 넣는다
        }
    }
    pthread_mutex_lock(&g_lock);
    while (g_done < 100) pthread_cond_wait(&g_changed, &g_lock);
    pthread_mutex_unlock(&g_lock);
    pool_stop();
    assert(g_done == 100);
}

// [역할] 작업자 1개가 막혀 있으면 큐가 POOL_QUEUE_SIZE 개에서 꽉 차고, 그다음 submit 은 -1.
//        문을 열면 pool_stop 이 큐에 남은 작업까지 다 처리한다 (안전한 종료).
static void test_full_queue_rejects(void)
{
    reset(0);
    assert(pool_start(1, counting_job) == 0);

    assert(pool_submit(-1) == 0);
    pthread_mutex_lock(&g_lock);
    while (g_started < 1) pthread_cond_wait(&g_changed, &g_lock);   // 작업자가 첫 일을 꺼낼 때까지
    pthread_mutex_unlock(&g_lock);

    for (int i = 0; i < POOL_QUEUE_SIZE; i++) assert(pool_submit(-1) == 0);
    assert(pool_submit(-1) == -1);   // 65번째: 자리가 없다

    pthread_mutex_lock(&g_lock);
    g_gate_open = 1;
    pthread_cond_broadcast(&g_changed);
    pthread_mutex_unlock(&g_lock);
    pool_stop();
    assert(g_done == 1 + POOL_QUEUE_SIZE);   // PR #6 과 달리 남은 작업을 버리지 않는다
}

// [역할] 작업자 수가 1..POOL_MAX_THREADS 밖이면 시작하지 않는다.
static void test_bad_worker_count(void)
{
    assert(pool_start(0, counting_job) == -1);
    assert(pool_start(POOL_MAX_THREADS + 1, counting_job) == -1);
}

int main(void)
{
    test_runs_every_job_once();
    test_full_queue_rejects();
    test_bad_worker_count();
    printf("test_pool: ok\n");
    return 0;
}
