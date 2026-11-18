#include "server.h"

#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "executor.h"
#include "http.h"
#include "statement.h"
#include "token.h"

// ---------------------------------------------------------------------------
// SQL → JSON
// ---------------------------------------------------------------------------

// execute 의 행 콜백이 받는 ctx. 첫 행 앞에는 쉼표를 안 붙이려고 개수를 센다.
typedef struct {
    Buf *out;
    int  rows;
} RowSink;

// CLI 의 print_row 자리. 행 하나를 JSON 배열로 바꿔 out 에 이어 붙인다.
static void json_row(const Value *fields, int field_count, void *ctx)
{
    RowSink *sink = ctx;
    if (sink->rows > 0) {
        buf_append(sink->out, ",");
    }
    json_put_row(sink->out, fields, field_count);
    sink->rows++;
}

// 엔진의 오류 코드를 HTTP 상태 코드로 바꾼다.
static int status_of(DbStatus st)
{
    switch (st) {
    case DB_OK:               return 200;
    case DB_ERR_NO_TABLE:     return 404;
    case DB_ERR_SYNTAX:
    case DB_ERR_TOO_LONG:
    case DB_ERR_NO_COLUMN:
    case DB_ERR_COLUMN_COUNT:
    case DB_ERR_TYPE:         return 400;   // 클라이언트가 보낸 SQL 이 틀렸다
    default:                  return 500;   // 스키마 파일·데이터 파일 문제 = 서버 탓
    }
}

// out 을 비우고 {"ok":false,"error":"..."} 로 다시 쓴 뒤 status 를 돌려준다.
// 실패하기 전까지 쌓아 둔 행은 여기서 버려진다.
static int reply_error(Buf *out, int status, const char *message)
{
    buf_free(out);
    buf_append(out, "{\"ok\":false,\"error\":");
    json_put_string(out, message);
    buf_append(out, "}");
    return status;
}

// 테이블이 이미 열려 있는가. Db.tables 는 쓰기 락 안에서만 바뀌므로 읽기 락을 쥐고 봐도 된다.
static int table_is_open(const Db *db, const char *name)
{
    for (int i = 0; i < db->table_count; i++) {
        if (strcmp(db->tables[i].name, name) == 0) {
            return 1;
        }
    }
    return 0;
}

// 문장에 맞는 락을 잡는다.
//   INSERT → 쓰기 락: next_id, B+ 트리, 파일 끝을 바꾼다. 혼자 들어간다.
//   SELECT → 읽기 락: 공유 상태를 읽기만 한다. SELECT 끼리는 동시에 들어간다.
// 예외: 처음 쓰는 테이블의 SELECT 는 execute 안의 db_get_table 이 테이블을 열면서
//       Db.tables 에 등록하고 B+ 트리를 만든다(= 쓰기). 그래서 이때만 쓰기 락으로 바꾼다.
static void lock_for(Server *s, const Statement *stmt)
{
    if (stmt->type == STMT_SELECT) {
        pthread_rwlock_rdlock(&s->db_lock);
        if (table_is_open(s->db, stmt->table)) {
            return;
        }
        // 읽기 락을 쥔 채로 쓰기 락을 달라고 하면 자기 자신을 기다리는 데드락이 된다.
        // 그래서 놓고 다시 잡는다. 그 사이에 다른 워커가 먼저 열어 두었어도
        // db_get_table 이 열린 것을 찾아서 돌려주므로 문제없다.
        pthread_rwlock_unlock(&s->db_lock);
    }
    pthread_rwlock_wrlock(&s->db_lock);
}

// 토큰이 ";" 하나뿐인 빈 문장인가
static int is_empty_statement(const Token *toks, int count)
{
    return count == 1 || (count == 2 && toks[0].type == TOK_SEMICOLON);
}

int server_handle_sql(Server *s, const char *sql, Buf *out)
{
    Token     toks[MAX_TOKENS];
    Token     rest[MAX_TOKENS];
    int       pos = 0;
    int       count = 0;
    DbError   err;
    DbStatus  st;

    // 1) 문장 하나를 토큰으로. 앞에 붙은 빈 문장(";")은 건너뛴다
    do {
        st = tokenize_next(sql, &pos, toks, MAX_TOKENS, &count, &err);
        if (st != DB_OK) {
            return reply_error(out, status_of(st), err.message);
        }
    } while (count == 2 && toks[0].type == TOK_SEMICOLON);
    if (count == 1) {
        return reply_error(out, 400, "Error: empty query");
    }

    // 2) 뒤에 문장이 더 있으면 거절한다 (요청 하나 = 문장 하나). 끝에 붙은 ";" 는 괜찮다
    int rest_pos = pos;
    int rest_count = 0;
    do {
        st = tokenize_next(sql, &rest_pos, rest, MAX_TOKENS, &rest_count, &err);
    } while (st == DB_OK && rest_count == 2 && rest[0].type == TOK_SEMICOLON);
    if (st != DB_OK || !is_empty_statement(rest, rest_count)) {
        return reply_error(out, 400, "Error: only one statement per request");
    }

    // 3) parse: 이 요청의 지역 변수만 쓴다 (공유 상태 없음 → 3겹에서도 락 밖)
    Statement stmt;
    if ((st = parse(toks, count, &stmt, &err)) != DB_OK) {
        return reply_error(out, status_of(st), err.message);
    }

    // 4) execute: 모든 워커가 함께 쓰는 Db 를 건드리므로 db_lock 으로 감싼다.
    //    락이 없으면 (실제로 깨뜨려 본 결과) 같은 id 가 두 번 붙고, INSERT 가 사라지고,
    //    재시작하면 "duplicate id" 로 테이블을 못 연다.
    //    락 안에서는 메모리 작업만 한다. 소켓을 읽고 쓰는 일은 락 밖에서 한다
    //    (느린 클라이언트 하나가 락을 쥔 채로 DB 전체를 멈추게 하면 안 된다).
    ExecResult res;
    RowSink    sink = { out, 0 };
    if (stmt.type == STMT_SELECT) {
        buf_append(out, "{\"ok\":true,\"rows\":[");   // 행은 콜백이 이 뒤에 이어 붙인다
    }
    lock_for(s, &stmt);
    st = execute(s->db, &stmt, json_row, &sink, &res, &err);
    if (st == DB_OK && stmt.type == STMT_INSERT) {
        // 200 을 보내기 전에 행을 stdio 버퍼(유저 공간)에서 커널로 넘긴다.
        // 이게 없으면 마지막 INSERT 가 버퍼에 남아 있다가 kill -9 / 크래시 때 사라진다.
        Table *t;
        if (db_get_table(s->db, stmt.table, &t, &err) == DB_OK && fflush(t->fp) != 0) {
            st = error_set(&err, DB_ERR_IO, -1, "cannot write to table '%s'", stmt.table);
        }
    }
    pthread_rwlock_unlock(&s->db_lock);   // 읽기 락이든 쓰기 락이든 푸는 함수는 같다.
                                          // 위에서 return 하는 길이 없어서 여기 한 곳에서만 푼다
    statement_free(&stmt);   // 실패해도 조건 트리는 해제한다
    if (st != DB_OK) {
        return reply_error(out, status_of(st), err.message);
    }

    // 5) 결과 마무리
    char tail[64];
    if (stmt.type == STMT_INSERT) {
        snprintf(tail, sizeof tail, "{\"ok\":true,\"inserted_id\":%lld}", res.inserted_id);
    } else {
        snprintf(tail, sizeof tail, "],\"row_count\":%d}", res.row_count);
    }
    buf_append(out, tail);
    if (out->failed) {
        return reply_error(out, 500, "Error: response too large or out of memory");
    }
    return 200;
}

// ---------------------------------------------------------------------------
// 연결 하나
// ---------------------------------------------------------------------------

void server_handle_client(int client_fd, void *ctx)
{
    Server     *s = ctx;
    HttpRequest req;   // 본문 8KB 를 담아서 스택에 두기엔 조금 크지만, 스레드 스택(8MB)에 충분히 들어간다
    Buf         out;
    int         status;

    buf_init(&out);

    // 아무것도 안 보내는 클라이언트가 이 연결을 영원히 붙잡지 못하게 recv 에 5초 제한을 건다
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    status = http_read_request(client_fd, &req);
    if (status != 0) {
        status = reply_error(&out, status, status == 413 ? "Error: request body too large"
                                                          : "Error: bad HTTP request");
    } else if (strcmp(req.path, "/query") != 0) {
        status = reply_error(&out, 404, "Error: not found (use POST /query)");
    } else if (strcmp(req.method, "POST") != 0) {
        status = reply_error(&out, 405, "Error: method not allowed (use POST)");
    } else {
        status = server_handle_sql(s, req.body, &out);
    }

    if (out.failed) {   // 오류 JSON 조차 못 만들었다 = 메모리가 없다
        static const char oom[] = "{\"ok\":false,\"error\":\"Error: out of memory\"}";
        http_write_response(client_fd, 500, oom, sizeof oom - 1);
    } else {
        http_write_response(client_fd, status, out.data, out.len);
    }
    buf_free(&out);
    close(client_fd);   // 닫지 않으면 fd 가 쌓여서 약 1000번째 연결부터 accept 가 실패한다 (EMFILE)
}

// ---------------------------------------------------------------------------
// 소켓 열기 + accept 루프
// ---------------------------------------------------------------------------

// socket → setsockopt → bind → listen. 듣는 소켓 fd, 실패하면 -1.
static int open_listen_socket(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);   // 전화기를 산다 (IPv4, TCP)
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    // 서버를 껐다 바로 켤 때 "Address already in use" 를 피한다
    // (닫힌 연결이 TIME_WAIT 로 잠시 포트를 잡고 있기 때문이다)
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);   // 모든 네트워크 카드에서 받는다
    addr.sin_port = htons((unsigned short)port); // 바이트 순서를 네트워크 순서(빅 엔디언)로

    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {   // 전화번호를 개통한다
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 128) != 0) {   // 영업 시작. accept 전에 대기 줄에 128명까지 세운다
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

int server_run(Db *db, int port, int thread_count)
{
    // s 는 이 함수의 지역 변수지만, 이 함수가 끝나지 않으므로 워커들이 계속 가리켜도 안전하다
    Server s;
    s.db = db;
    s.listen_fd = open_listen_socket(port);
    if (s.listen_fd < 0) {
        return -1;
    }
    // 워커가 뜨기 전에 만들어 둔다.
    // 기본 rwlock 은 읽기 우선이라 SELECT 가 끊임없이 오면 INSERT 가 영원히 못 들어갈 수 있다(기아).
    // 쓰기를 기다리는 워커가 있으면 새 읽기를 막는 glibc 옵션으로 그걸 막는다.
    pthread_rwlockattr_t attr;
    pthread_rwlockattr_init(&attr);
    pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    pthread_rwlock_init(&s.db_lock, &attr);
    pthread_rwlockattr_destroy(&attr);
    if (threadpool_start(&s.pool, thread_count, server_handle_client, &s) != 0) {
        fprintf(stderr, "Error: cannot start %d workers (1..%d)\n", thread_count, MAX_WORKERS);
        pthread_rwlock_destroy(&s.db_lock);
        close(s.listen_fd);
        return -1;
    }
    printf("listening on port %d (%d workers)\n", port, thread_count);
    fflush(stdout);

    for (;;) {
        // 연결이 올 때까지 잠들어 기다리다가(CPU 를 안 쓴다), 오면 이 손님 전용 fd 를 새로 받는다
        int client_fd = accept(s.listen_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno != EINTR) {
                perror("accept");   // EMFILE 등. 서버는 계속 돈다
            }
            continue;
        }
        // 2겹: 직접 처리하지 않고 큐에 넣기만 한다. 바로 다음 accept 로 돌아가므로
        // 느린 손님 하나가 입구를 막지 못한다.
        if (threadpool_submit(&s.pool, client_fd) != 0) {
            // 큐가 꽉 찼다 = 워커가 모두 바쁘고 64명이 이미 기다린다. 기다리게 하지 않고 바로 거절한다.
            static const char busy[] = "{\"ok\":false,\"error\":\"Error: server busy\"}";
            http_write_response(client_fd, 503, busy, sizeof busy - 1);
            close(client_fd);
        }
    }
}
