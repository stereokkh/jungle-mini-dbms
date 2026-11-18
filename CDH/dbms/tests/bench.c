// 부하 생성기: 클라이언트 스레드 여러 개가 서버에 같은 SQL 을 동시에 계속 보내고 속도를 잰다.
//
//   사용법: ./build/bench <포트> <동시 클라이언트 수> <총 요청 수> '<SQL>'
//   출력:   한 줄 "처리량(req/s) 평균지연(ms) p99지연(ms) 오류수"
//
// 요청마다 연결을 새로 맺는다 (서버가 Connection: close 라서).
// 서버 스레드 수를 바꿔 가며 같은 부하를 주면 스레드 풀의 효과를 숫자로 볼 수 있다 (bench.sh).

#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int    g_port;
static int    g_per_client;          // 클라이언트 하나가 보낼 요청 수
static char   g_request[8192];       // 모든 요청이 똑같으므로 미리 한 번 만들어 둔다
static size_t g_request_len;
static double *g_latency;            // 요청마다 걸린 시간(ms). 클라이언트 i 는 자기 구간에만 쓴다
static int    g_errors;
static pthread_mutex_t g_err_lock = PTHREAD_MUTEX_INITIALIZER;

// [역할] 지금 시각을 ms 로 (단조 시계: 시스템 시간이 바뀌어도 거꾸로 가지 않는다)
static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

// [역할] 요청 하나: 연결 → 보내기 → 서버가 닫을 때까지 받기 → 닫기.
// [반환] 0 = 응답이 "HTTP/1.1 200" 이고 JSON-RPC result 가 있음, -1 = 실패
static int one_request(void)
{
    char buf[65536];
    size_t got = 0;
    ssize_t n;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};

    if (fd < 0) return -1;
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)g_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0 ||
        send(fd, g_request, g_request_len, MSG_NOSIGNAL) != (ssize_t)g_request_len) {
        close(fd);
        return -1;
    }
    // 응답 앞부분만 남기고 나머지는 버리며 끝까지 읽는다
    while ((n = recv(fd, buf + got, sizeof buf - 1 - got, 0)) > 0) {
        if (got + (size_t)n < sizeof buf - 1) got += (size_t)n;
    }
    close(fd);
    buf[got] = '\0';
    return (strncmp(buf, "HTTP/1.1 200", 12) == 0 && strstr(buf, "\"result\"") != NULL) ? 0 : -1;
}

// [역할] 클라이언트 스레드 하나: 자기 몫만큼 요청을 보내며 하나하나 시간을 잰다.
static void *client(void *arg)
{
    int id = (int)(long)arg;
    for (int i = 0; i < g_per_client; i++) {
        double t0 = now_ms();
        int bad = one_request();
        g_latency[id * g_per_client + i] = now_ms() - t0;
        if (bad) {
            pthread_mutex_lock(&g_err_lock);
            g_errors++;
            pthread_mutex_unlock(&g_err_lock);
        }
    }
    return NULL;
}

// [역할] qsort 용 비교 함수 (지연 시간 오름차순)
static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

// [역할] 시작점.
// [순서] 인자 읽기 → 요청 문자열 만들기 → 클라이언트 스레드 띄우고 전체 시간 재기
//        → 지연 시간 정렬해 평균·p99 계산 → 한 줄 출력
int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s <port> <clients> <total> '<sql>'\n", argv[0]);
        return 1;
    }
    g_port = atoi(argv[1]);
    int clients = atoi(argv[2]);
    int total = atoi(argv[3]);
    if (clients < 1 || total < clients) return 1;
    g_per_client = total / clients;
    total = g_per_client * clients;

    // SQL 에 " 나 \ 가 없다고 가정한 테스트용 도구라 이스케이프는 하지 않는다
    char body[4096];
    int blen = snprintf(body, sizeof body,
                        "{\"jsonrpc\":\"2.0\",\"method\":\"query\",\"params\":[\"%s\"],\"id\":1}", argv[4]);
    g_request_len = (size_t)snprintf(g_request, sizeof g_request,
                                     "POST /rpc HTTP/1.1\r\nHost: localhost\r\n"
                                     "Content-Type: application/json\r\nContent-Length: %d\r\n\r\n%s",
                                     blen, body);

    g_latency = calloc((size_t)total, sizeof *g_latency);
    pthread_t *th = calloc((size_t)clients, sizeof *th);
    if (g_latency == NULL || th == NULL) return 1;

    double t0 = now_ms();
    for (int i = 0; i < clients; i++) pthread_create(&th[i], NULL, client, (void *)(long)i);
    for (int i = 0; i < clients; i++) pthread_join(th[i], NULL);
    double elapsed = now_ms() - t0;

    double sum = 0;
    for (int i = 0; i < total; i++) sum += g_latency[i];
    qsort(g_latency, (size_t)total, sizeof *g_latency, cmp_double);

    printf("%8.0f req/s   avg %7.2f ms   p99 %7.2f ms   errors %d\n",
           total / (elapsed / 1000.0), sum / total, g_latency[(int)(total * 0.99)], g_errors);
    free(g_latency);
    free(th);
    return g_errors ? 2 : 0;
}
