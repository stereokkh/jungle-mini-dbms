#include <assert.h>
#include <pthread.h>
#include <stdio.h>

#include "threadpool.h"

// 작업 함수가 함께 쓰는 카운터. 워커 여러 개가 동시에 올리므로 자기 락이 필요하다.
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t  changed;
    int             done;      // 끝난 작업 수
    int             started;   // 시작한 작업 수
    int             gate_open; // 0 이면 작업이 문 앞에서 기다린다 (큐를 채우는 테스트용)
} Counter;

static void counting_job(int fd, void *ctx)
{
    (void)fd;   // 가짜 fd(-1)라서 닫을 것이 없다
    Counter *c = ctx;

    pthread_mutex_lock(&c->lock);
    c->started++;
    pthread_cond_broadcast(&c->changed);
    while (!c->gate_open) {
        pthread_cond_wait(&c->changed, &c->lock);
    }
    c->done++;
    pthread_cond_broadcast(&c->changed);
    pthread_mutex_unlock(&c->lock);
}

static void counter_init(Counter *c, int gate_open)
{
    pthread_mutex_init(&c->lock, NULL);
    pthread_cond_init(&c->changed, NULL);
    c->done = 0;
    c->started = 0;
    c->gate_open = gate_open;
}

static void counter_destroy(Counter *c)
{
    pthread_cond_destroy(&c->changed);
    pthread_mutex_destroy(&c->lock);
}

// 작업 100개를 넣으면 워커 4개가 나눠서 정확히 100번 실행한다
static void test_runs_every_job_once(void)
{
    ThreadPool p;
    Counter    c;

    counter_init(&c, 1);
    assert(threadpool_start(&p, 4, counting_job, &c) == 0);
    for (int i = 0; i < 100; i++) {
        while (threadpool_submit(&p, -1) != 0) {
            // 큐(64칸)가 잠깐 찼으면 워커가 비울 때까지 다시 넣는다
        }
    }
    pthread_mutex_lock(&c.lock);
    while (c.done < 100) {
        pthread_cond_wait(&c.changed, &c.lock);
    }
    pthread_mutex_unlock(&c.lock);
    threadpool_stop(&p);

    assert(c.done == 100);
    counter_destroy(&c);
}

// 워커 1개가 막혀 있으면 큐가 QUEUE_CAP 개에서 꽉 차고, 그다음 submit 은 -1
static void test_full_queue_rejects(void)
{
    ThreadPool p;
    Counter    c;

    counter_init(&c, 0);   // 문을 닫아 둔다
    assert(threadpool_start(&p, 1, counting_job, &c) == 0);

    assert(threadpool_submit(&p, -1) == 0);
    pthread_mutex_lock(&c.lock);
    while (c.started < 1) {   // 워커가 첫 작업을 꺼내 문 앞에서 기다릴 때까지
        pthread_cond_wait(&c.changed, &c.lock);
    }
    pthread_mutex_unlock(&c.lock);

    for (int i = 0; i < QUEUE_CAP; i++) {
        assert(threadpool_submit(&p, -1) == 0);
    }
    assert(threadpool_submit(&p, -1) == -1);   // 65번째: 자리가 없다

    pthread_mutex_lock(&c.lock);
    c.gate_open = 1;
    pthread_cond_broadcast(&c.changed);
    pthread_mutex_unlock(&c.lock);
    threadpool_stop(&p);   // 남은 작업이 있어도 멈추고, 남은 fd 를 닫는다 (asan 으로 누수 확인)
    counter_destroy(&c);
}

static void test_bad_worker_count(void)
{
    ThreadPool p;
    Counter    c;

    counter_init(&c, 1);
    assert(threadpool_start(&p, 0, counting_job, &c) == -1);
    assert(threadpool_start(&p, MAX_WORKERS + 1, counting_job, &c) == -1);
    counter_destroy(&c);
}

int main(void)
{
    test_runs_every_job_once();
    test_full_queue_rejects();
    test_bad_worker_count();
    printf("test_threadpool: ok\n");
    return 0;
}
