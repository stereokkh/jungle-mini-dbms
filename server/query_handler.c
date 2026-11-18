#include "query_handler.h"

#include <jansson.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sql.h"

#define MAX_BODY_BYTES (16 * 1024)
#define QUERY_WORKER_COUNT 2
#define QUERY_QUEUE_CAPACITY 4

typedef struct {
    unsigned int status;
    char *body;
} QueryResponse;

typedef struct {
    char body[MAX_BODY_BYTES];
    size_t length;
    bool too_large;
    struct MHD_Connection *connection; // 작업 큐에 들어간 요청에만 설정
    QueryResponse response;
} Request;

struct QueryPool {
    Db *db;
    pthread_t workers[QUERY_WORKER_COUNT];
    pthread_mutex_t mutex; // 작업큐 뮤텍스. 여러 스레드가 공유 상태를 바꾸는 동안 보호
    pthread_cond_t work_available; // 큐가 비었을 때 워커가 잠드는 조건변수
    pthread_cond_t requests_finished; // 종료 중인 메인스레드가 기다리는 조건 변수
    Request *queued[QUERY_QUEUE_CAPACITY]; // 대기 중인 요청의 Request *를 보관하는 배열
    size_t head;         // 워커가 다음에 꺼낼 칸
    size_t tail;         // HTTP 스레드가 다음에 넣을 칸
    size_t queued_count; // 큐에 실행 대기 중인 작업 수
    size_t pending;      // 큐에 들어간 뒤 HTTP 완료 콜백까지 남아 있는 요청 수
    bool closing;
};

typedef struct {
    json_t *rows;
    bool failed;
} QueryRows;

static enum MHD_Result send_body(struct MHD_Connection *connection, unsigned int status,
                                 const char *body)
{
    struct MHD_Response *response = MHD_create_response_from_buffer(
        strlen(body), (void *)body, MHD_RESPMEM_MUST_COPY);
    if (response == NULL) {
        return MHD_NO;
    }

    enum MHD_Result sent = MHD_NO;
    if (MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, "application/json") == MHD_YES
        && (status != MHD_HTTP_METHOD_NOT_ALLOWED
            || MHD_add_response_header(response, MHD_HTTP_HEADER_ALLOW, "POST") == MHD_YES)) {
        sent = MHD_queue_response(connection, status, response);
    }
    MHD_destroy_response(response);
    return sent;
}

static QueryResponse make_json_response(unsigned int status, json_t *value)
{
    char *body = value == NULL ? NULL : json_dumps(value, JSON_COMPACT);
    json_decref(value);
    return (QueryResponse){ .status = status, .body = body };
}

static QueryResponse error_response(unsigned int status, const char *message)
{
    return make_json_response(status, json_pack("{s:s}", "error", message));
}

static enum MHD_Result send_query_response(struct MHD_Connection *connection,
                                           const QueryResponse *response)
{
    if (response->body == NULL) {
        return send_body(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
                         "{\"error\":\"could not create response\"}");
    }
    return send_body(connection, response->status, response->body);
}

static enum MHD_Result send_error(struct MHD_Connection *connection, unsigned int status,
                                  const char *message)
{
    QueryResponse response = error_response(status, message);
    enum MHD_Result sent = send_query_response(connection, &response);
    free(response.body);
    return sent;
}

static void collect_row(const Value *fields, int count, void *ctx)
{
    QueryRows *result = ctx;
    if (result->failed) {
        return;
    }
    json_t *row = json_array();
    if (row == NULL) {
        result->failed = true;
        return;
    }
    for (int i = 0; i < count; i++) {
        json_t *field = fields[i].type == VAL_INT ? json_integer(fields[i].i)
                                                 : json_string(fields[i].s);
        if (field == NULL || json_array_append_new(row, field) != 0) {
            result->failed = true;
            json_decref(row);
            return;
        }
    }
    result->failed = (json_array_append_new(result->rows, row) != 0);
}

static QueryResponse db_error_response(const DbError *err)
{
    // 저장소에서 읽은 기존 행이 너무 긴 경우에는 SQL 내 위치가 없다.
    if (err->code == DB_ERR_IO || err->code == DB_ERR_SCHEMA
        || (err->code == DB_ERR_TOO_LONG && err->pos < 0)) {
        fprintf(stderr, "%s\n", err->message);
        return error_response(MHD_HTTP_INTERNAL_SERVER_ERROR, "database operation failed");
    }
    return error_response(MHD_HTTP_BAD_REQUEST, err->message);
}

static QueryResponse run_query(Db *db, const Request *request)
{
    json_t *input = json_loadb(request->body, request->length, JSON_REJECT_DUPLICATES, NULL);
    if (input == NULL) {
        return error_response(MHD_HTTP_BAD_REQUEST, "invalid JSON");
    }
    const char *sql = json_string_value(json_object_get(input, "sql"));
    if (sql == NULL) {
        json_decref(input);
        return error_response(MHD_HTTP_BAD_REQUEST, "sql must be a string");
    }

    QueryRows collected = { .rows = json_array() };
    if (collected.rows == NULL) {
        json_decref(input);
        return error_response(MHD_HTTP_INTERNAL_SERVER_ERROR, "could not collect results");
    }
    StmtType type;
    ExecResult result;
    DbError err;
    // sql 은 input 에서 빌린 문자열이다. 실행이 끝날 때까지 input 을 유지한다.
    DbStatus status = run_one_sql(db, sql, collect_row, &collected, &type, &result, &err);
    json_decref(input);

    QueryResponse response;
    if (status != DB_OK) {
        response = db_error_response(&err);
    } else if (collected.failed) {
        response = error_response(MHD_HTTP_INTERNAL_SERVER_ERROR, "could not collect results");
    } else if (type == STMT_INSERT) {
        response = make_json_response(MHD_HTTP_OK,
                                      json_pack("{s:I}", "inserted_id", (json_int_t)result.inserted_id));
    } else {
        response = make_json_response(MHD_HTTP_OK,
                                      json_pack("{s:O,s:i}", "rows", collected.rows,
                                                "row_count", result.row_count));
    }
    json_decref(collected.rows);
    return response;
}

static void *query_worker(void *arg)
{
    QueryPool *pool = arg;
    pthread_mutex_lock(&pool->mutex);
    for (;;) {
        while (pool->queued_count == 0 && !pool->closing) {
            pthread_cond_wait(&pool->work_available, &pool->mutex);
        }
        if (pool->queued_count == 0) {
            break;
        }

        Request *request = pool->queued[pool->head];
        struct MHD_Connection *connection = request->connection;
        pool->queued[pool->head] = NULL;
        pool->head = (pool->head + 1) % QUERY_QUEUE_CAPACITY;
        pool->queued_count--;
        pthread_mutex_unlock(&pool->mutex);

        QueryResponse response = run_query(pool->db, request);

        pthread_mutex_lock(&pool->mutex);
        request->response = response;
        // 재개 뒤에는 완료 콜백이 request 를 해제할 수 있다.
        pthread_mutex_unlock(&pool->mutex);
        MHD_resume_connection(connection);
        pthread_mutex_lock(&pool->mutex);
    }
    pthread_mutex_unlock(&pool->mutex);
    return NULL;
}

static void join_query_workers(QueryPool *pool, size_t worker_count)
{
    pthread_mutex_lock(&pool->mutex);
    pool->closing = true;
    pthread_cond_broadcast(&pool->work_available);
    pthread_mutex_unlock(&pool->mutex);

    for (size_t i = 0; i < worker_count; i++) {
        pthread_join(pool->workers[i], NULL);
    }
}

QueryPool *query_pool_start(Db *db)
{
    QueryPool *pool = calloc(1, sizeof(*pool));
    if (pool == NULL) {
        return NULL;
    }
    pool->db = db;
    if (pthread_mutex_init(&pool->mutex, NULL) != 0) {
        goto free_pool;
    }
    if (pthread_cond_init(&pool->work_available, NULL) != 0) {
        goto destroy_mutex;
    }
    if (pthread_cond_init(&pool->requests_finished, NULL) != 0) {
        goto destroy_work_available;
    }
    for (size_t i = 0; i < QUERY_WORKER_COUNT; i++) {
        if (pthread_create(&pool->workers[i], NULL, query_worker, pool) != 0) {
            // 생성에 성공한 워커만 기다린 뒤 공유 자원을 해제한다.
            join_query_workers(pool, i);
            goto destroy_requests_finished;
        }
    }
    return pool;

destroy_requests_finished:
    pthread_cond_destroy(&pool->requests_finished);
destroy_work_available:
    pthread_cond_destroy(&pool->work_available);
destroy_mutex:
    pthread_mutex_destroy(&pool->mutex);
free_pool:
    free(pool);
    return NULL;
}

void query_pool_drain(QueryPool *pool)
{
    join_query_workers(pool, QUERY_WORKER_COUNT);

    pthread_mutex_lock(&pool->mutex);
    while (pool->pending != 0) {
        pthread_cond_wait(&pool->requests_finished, &pool->mutex);
    }
    pthread_mutex_unlock(&pool->mutex);
}

void query_pool_destroy(QueryPool *pool)
{
    pthread_cond_destroy(&pool->requests_finished);
    pthread_cond_destroy(&pool->work_available);
    pthread_mutex_destroy(&pool->mutex);
    free(pool);
}

static bool try_enqueue(QueryPool *pool, struct MHD_Connection *connection, Request *request)
{
    pthread_mutex_lock(&pool->mutex);
    bool accepted = !pool->closing && pool->queued_count < QUERY_QUEUE_CAPACITY;
    if (accepted) {
        // 연결을 먼저 멈춰야 워커가 작업을 마치기 전에 요청 상태가 해제되지 않는다.
        MHD_suspend_connection(connection);
        request->connection = connection;
        pool->queued[pool->tail] = request;
        pool->tail = (pool->tail + 1) % QUERY_QUEUE_CAPACITY;
        pool->queued_count++;
        pool->pending++;
        pthread_cond_signal(&pool->work_available);
    }
    pthread_mutex_unlock(&pool->mutex);
    return accepted;
}

enum MHD_Result handle_request(void *cls, struct MHD_Connection *connection,
                              const char *url, const char *method, const char *version,
                              const char *upload_data, size_t *upload_size, void **request_state)
{
    (void)version;
    if (strcmp(url, "/query") != 0) {
        return send_error(connection, MHD_HTTP_NOT_FOUND, "route not found");
    }
    if (strcmp(method, MHD_HTTP_METHOD_POST) != 0) {
        return send_error(connection, MHD_HTTP_METHOD_NOT_ALLOWED, "use POST");
    }
    if (*request_state == NULL) {
        // 첫 호출은 요청 상태를 설정한다. 본문이 오면 같은 핸들러가 다시 불린다.
        *request_state = calloc(1, sizeof(Request));
        if (*request_state == NULL) {
            return send_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "could not accept request");
        }
        return MHD_YES;
    }

    Request *request = *request_state;
    if (*upload_size != 0) {
        if (*upload_size > sizeof(request->body) - request->length) {
            request->too_large = true;
        } else if (!request->too_large) {
            // 본문을 버퍼에 복사
            memcpy(request->body + request->length, upload_data, *upload_size);
            request->length += *upload_size;
        }
        *upload_size = 0;
        return MHD_YES;
    }
    if (request->too_large) {
        return send_error(connection, 413, "request body exceeds 16 KiB");
    }
    QueryPool *pool = cls;
    if (request->connection != NULL) {
        // 워커가 응답을 기록한 뒤 연결을 재개했다.
        pthread_mutex_lock(&pool->mutex);
        QueryResponse response = request->response;
        pthread_mutex_unlock(&pool->mutex);
        return send_query_response(connection, &response);
    }
    if (!try_enqueue(pool, connection, request)) {
        return send_error(connection, MHD_HTTP_SERVICE_UNAVAILABLE, "query worker unavailable");
    }
    return MHD_YES;
}

void finish_request(void *cls, struct MHD_Connection *connection,
                    void **request_state, enum MHD_RequestTerminationCode reason)
{
    (void)connection;
    (void)reason;
    QueryPool *pool = cls;
    Request *request = *request_state;
    if (request != NULL) {
        if (request->connection != NULL) {
            pthread_mutex_lock(&pool->mutex);
            free(request->response.body);
            pool->pending--;
            if (pool->pending == 0) {
                pthread_cond_signal(&pool->requests_finished);
            }
            pthread_mutex_unlock(&pool->mutex);
        }
        free(request);
    }
    *request_state = NULL;
}
