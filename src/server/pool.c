#include "pool.h"

#include <pthread.h>

// 큐는 고정 크기 원형 배열이다. head 에서 꺼내고 (head + count) 자리에 넣는다.
static int             g_queue[POOL_QUEUE_SIZE];
static int             g_head, g_count;
static int             g_stopping;            // pool_stop 이 부르면 1
static pthread_mutex_t g_lock      = PTHREAD_MUTEX_INITIALIZER;   // 위 네 변수를 지킨다
static pthread_cond_t  g_not_empty = PTHREAD_COND_INITIALIZER;    // "큐에 일이 들어왔다" 신호

static pthread_t   g_threads[POOL_MAX_THREADS];
static int         g_nthreads;
static PoolHandler g_handler;

// [역할] 작업자 스레드 하나의 본체. 큐에서 연결을 꺼내 handler 로 처리하기를 반복한다.
// [순서] 락 → 큐가 빌 동안 cond 에서 잠듦 → fd 하나 꺼냄 → 락 해제 → handler(fd) (락 밖에서!)
// [기능] 종료 중이고 큐도 비었으면 루프를 빠져나와 스레드가 끝난다.
//        handler 를 락 밖에서 부르는 것이 핵심이다. 안에서 부르면 한 번에 하나만 처리하게 된다.
static void *worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_lock);
        // while 인 이유: 깨어났는데 다른 작업자가 먼저 가져갔을 수 있다 (가짜 깨어남 포함)
        while (g_count == 0 && !g_stopping) {
            pthread_cond_wait(&g_not_empty, &g_lock);   // 락을 풀고 잠들었다가, 깨면 다시 잡는다
        }
        if (g_count == 0) {   // 종료 중 + 남은 일 없음
            pthread_mutex_unlock(&g_lock);
            return NULL;
        }
        int fd = g_queue[g_head];
        g_head = (g_head + 1) % POOL_QUEUE_SIZE;
        g_count--;
        pthread_mutex_unlock(&g_lock);

        g_handler(fd);
    }
}

// [역할] 작업자 스레드를 nthreads 개 만든다.
// [반환] 0 = 성공, -1 = 개수가 범위 밖이거나 스레드 생성 실패 (이미 만든 스레드는 정리한다)
int pool_start(int nthreads, PoolHandler handler)
{
    if (nthreads < 1 || nthreads > POOL_MAX_THREADS) return -1;
    g_handler = handler;
    g_head = g_count = g_stopping = 0;

    for (g_nthreads = 0; g_nthreads < nthreads; g_nthreads++) {
        if (pthread_create(&g_threads[g_nthreads], NULL, worker, NULL) != 0) {
            pool_stop();
            return -1;
        }
    }
    return 0;
}

// [역할] accept 한 연결을 큐 뒤에 넣고 작업자 하나를 깨운다.
// [반환] 0 = 넣음, -1 = 큐가 꽉 차서 못 넣음 (작업자들이 모두 바쁘고 대기열도 가득)
int pool_submit(int fd)
{
    pthread_mutex_lock(&g_lock);
    if (g_count == POOL_QUEUE_SIZE) {
        pthread_mutex_unlock(&g_lock);
        return -1;
    }
    g_queue[(g_head + g_count) % POOL_QUEUE_SIZE] = fd;
    g_count++;
    pthread_cond_signal(&g_not_empty);   // 잠든 작업자 하나만 깨우면 충분하다
    pthread_mutex_unlock(&g_lock);
    return 0;
}

// [역할] 풀을 끈다.
// [순서] 종료 플래그 세움 → 잠든 작업자 모두 깨움(broadcast) → 모든 작업자가 끝날 때까지 join
// [기능] 작업자는 큐에 남은 연결을 마저 처리한 뒤 끝나므로, 받아 둔 요청은 버리지 않는다.
void pool_stop(void)
{
    pthread_mutex_lock(&g_lock);
    g_stopping = 1;
    pthread_cond_broadcast(&g_not_empty);
    pthread_mutex_unlock(&g_lock);

    for (int i = 0; i < g_nthreads; i++) {
        pthread_join(g_threads[i], NULL);
    }
    g_nthreads = 0;
}
