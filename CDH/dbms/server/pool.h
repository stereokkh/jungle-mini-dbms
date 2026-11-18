// 스레드 풀: 작업자 스레드 N 개가 "연결 큐" 에서 연결을 하나씩 꺼내 처리한다.
//
//   main(accept) ──pool_submit(fd)──▶ [ 큐: fd fd fd ... ] ──▶ 작업자 1..N ──▶ handler(fd)
//
// 큐는 mutex 로 보호하고, 큐가 비면 작업자는 조건 변수(cond)에서 잠들어 기다린다.
#ifndef POOL_H
#define POOL_H

#define POOL_MAX_THREADS 64
#define POOL_QUEUE_SIZE  64   // 처리 대기 중인 연결을 이만큼까지 쌓아 둔다

// 작업자가 연결 하나마다 부르는 함수. 연결을 다 쓰면 close 까지 해야 한다.
typedef void (*PoolHandler)(int fd);

// 작업자 스레드 nthreads 개를 띄운다. 0 = 성공, -1 = 실패
int pool_start(int nthreads, PoolHandler handler);

// 연결 fd 를 큐에 넣고 잠든 작업자 하나를 깨운다. 0 = 성공, -1 = 큐가 꽉 참(부른 쪽이 503 처리)
int pool_submit(int fd);

// 새 연결은 그만 받고, 큐에 남은 연결까지 다 처리한 뒤 작업자들이 끝나기를 기다린다.
void pool_stop(void);

#endif
