#include "threadpool.h"

#include <unistd.h>

// 워커 스레드 하나가 평생 도는 루프.
static void *worker_main(void *arg)
{
    ThreadPool *p = arg;

    for (;;) {
        pthread_mutex_lock(&p->lock);

        // 일이 없으면 잠든다. cond_wait 는 "락을 풀고 잠들기"를 한 번에 하고,
        // 깨어날 때 락을 다시 잡은 상태로 돌아온다.
        // if 가 아니라 while 인 이유:
        //   1) 깨어났는데 다른 워커가 먼저 일을 가져가 큐가 다시 비어 있을 수 있다
        //   2) 아무도 signal 하지 않았는데 깨어나는 경우(spurious wakeup)가 허용돼 있다
        // 그래서 깨어날 때마다 조건을 다시 확인한다.
        while (p->count == 0 && !p->stopping) {
            pthread_cond_wait(&p->not_empty, &p->lock);
        }
        if (p->stopping) {
            pthread_mutex_unlock(&p->lock);
            return NULL;
        }

        // 큐 앞에서 fd 하나를 꺼낸다
        int fd = p->queue[p->head];
        p->head = (p->head + 1) % QUEUE_CAP;
        p->count--;

        pthread_mutex_unlock(&p->lock);

        // 락을 푼 뒤에 일을 한다. 락을 잡은 채로 하면 일하는 동안 다른 워커가
        // 큐에서 아무것도 못 꺼내서, 스레드가 여러 개여도 한 번에 하나씩만 돈다.
        p->fn(fd, p->ctx);
    }
}

int threadpool_start(ThreadPool *p, int thread_count, JobFn fn, void *ctx)
{
    if (thread_count < 1 || thread_count > MAX_WORKERS) {
        return -1;
    }
    p->thread_count = 0;
    p->head = 0;
    p->count = 0;
    p->stopping = 0;
    p->fn = fn;
    p->ctx = ctx;
    if (pthread_mutex_init(&p->lock, NULL) != 0) {
        return -1;
    }
    if (pthread_cond_init(&p->not_empty, NULL) != 0) {
        pthread_mutex_destroy(&p->lock);
        return -1;
    }
    for (int i = 0; i < thread_count; i++) {
        if (pthread_create(&p->threads[i], NULL, worker_main, p) != 0) {
            threadpool_stop(p);   // 이미 띄운 것만 정리한다
            return -1;
        }
        p->thread_count++;
    }
    return 0;
}

int threadpool_submit(ThreadPool *p, int client_fd)
{
    pthread_mutex_lock(&p->lock);
    if (p->count == QUEUE_CAP || p->stopping) {
        pthread_mutex_unlock(&p->lock);
        return -1;
    }
    p->queue[(p->head + p->count) % QUEUE_CAP] = client_fd;   // 큐 뒤에 넣는다
    p->count++;
    pthread_cond_signal(&p->not_empty);   // 잠든 워커 하나만 깨운다 (일이 하나 들어왔으니)
    pthread_mutex_unlock(&p->lock);
    return 0;
}

void threadpool_stop(ThreadPool *p)
{
    pthread_mutex_lock(&p->lock);
    p->stopping = 1;
    pthread_cond_broadcast(&p->not_empty);   // 잠든 워커 모두 깨운다 (모두 끝나야 하니까)
    pthread_mutex_unlock(&p->lock);

    for (int i = 0; i < p->thread_count; i++) {
        pthread_join(p->threads[i], NULL);   // 하던 일을 마치고 루프를 나올 때까지 기다린다
    }
    p->thread_count = 0;

    // 워커가 못 집어 간 연결은 응답 없이 닫는다
    while (p->count > 0) {
        close(p->queue[p->head]);
        p->head = (p->head + 1) % QUEUE_CAP;
        p->count--;
    }
    pthread_cond_destroy(&p->not_empty);
    pthread_mutex_destroy(&p->lock);
}
