#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>

#include "db.h"
#include "query_handler.h"

#define SERVER_PORT 8080

int main(int argc, char **argv)
{
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [data-dir]\n", argv[0]);
        return 1;
    }

    // 새 스레드들이 신호 마스크를 상속한다. 종료 신호는 메인에서 받는다.
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &stop_signals, NULL) != 0) {
        fprintf(stderr, "Could not block shutdown signals\n");
        return 1;
    }

    Db db;
    DbError err;
    const char *data_dir = argc == 2 ? argv[1] : "data";
    if (db_open(data_dir, &db, &err) != DB_OK) {
        fprintf(stderr, "%s\n", err.message);
        return 1;
    }
    QueryPool *pool = query_pool_start(&db);
    if (pool == NULL) {
        fprintf(stderr, "Could not start SQL worker pool\n");
        db_close(&db);
        return 1;
    }

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(SERVER_PORT),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };
    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD | MHD_ALLOW_SUSPEND_RESUME,
        SERVER_PORT, NULL, NULL, handle_request, pool,
        MHD_OPTION_SOCK_ADDR, (struct sockaddr *)&address,
        MHD_OPTION_CONNECTION_TIMEOUT, 15u,
        MHD_OPTION_NOTIFY_COMPLETED, finish_request, pool,
        MHD_OPTION_END);
    if (daemon == NULL) {
        fprintf(stderr, "Could not listen on 127.0.0.1:%d\n", SERVER_PORT);
        query_pool_drain(pool);
        query_pool_destroy(pool);
        db_close(&db);
        return 1;
    }

    fprintf(stderr, "Listening on http://127.0.0.1:%d/query\n", SERVER_PORT);
    int signal_number;
    int wait_status = sigwait(&stop_signals, &signal_number);
    query_pool_drain(pool);
    MHD_stop_daemon(daemon);
    query_pool_destroy(pool);
    db_close(&db);
    return wait_status == 0 ? 0 : 1;
}
