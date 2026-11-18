#include <arpa/inet.h>
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

    // HTTP 스레드가 신호 마스크를 상속한다. 메인에서 신호를 기다린 뒤 자원을 닫는다.
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &stop_signals, NULL) != 0) {
        perror("sigprocmask");
        return 1;
    }

    Db db;
    DbError err;
    const char *data_dir = argc == 2 ? argv[1] : "data";
    if (db_open(data_dir, &db, &err) != DB_OK) {
        fprintf(stderr, "%s\n", err.message);
        return 1;
    }

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(SERVER_PORT),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK)
    };
    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD, SERVER_PORT, NULL, NULL, handle_request, &db,
        MHD_OPTION_SOCK_ADDR, (struct sockaddr *)&address,
        MHD_OPTION_CONNECTION_TIMEOUT, 15u,
        MHD_OPTION_NOTIFY_COMPLETED, finish_request, NULL,
        MHD_OPTION_END);
    if (daemon == NULL) {
        fprintf(stderr, "Could not listen on 127.0.0.1:%d\n", SERVER_PORT);
        db_close(&db);
        return 1;
    }

    fprintf(stderr, "Listening on http://127.0.0.1:%d/query\n", SERVER_PORT);
    int signal_number;
    int wait_status = sigwait(&stop_signals, &signal_number);
    MHD_stop_daemon(daemon);
    db_close(&db);
    return wait_status == 0 ? 0 : 1;
}
