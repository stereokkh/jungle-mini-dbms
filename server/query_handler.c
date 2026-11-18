#include "query_handler.h"

#include <jansson.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sql.h"

#define MAX_BODY_BYTES (16 * 1024)

typedef struct {
    char body[MAX_BODY_BYTES];
    size_t length;
    bool too_large;
} Request;

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

static enum MHD_Result send_json(struct MHD_Connection *connection, unsigned int status,
                                 json_t *value)
{
    char *body = value == NULL ? NULL : json_dumps(value, JSON_COMPACT);
    json_decref(value);
    if (body == NULL) {
        return send_body(connection, MHD_HTTP_INTERNAL_SERVER_ERROR,
                         "{\"error\":\"could not create response\"}");
    }
    enum MHD_Result sent = send_body(connection, status, body);
    free(body);
    return sent;
}

static enum MHD_Result send_error(struct MHD_Connection *connection, unsigned int status,
                                  const char *message)
{
    return send_json(connection, status, json_pack("{s:s}", "error", message));
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

static enum MHD_Result send_db_error(struct MHD_Connection *connection, const DbError *err)
{
    // 저장소에서 읽은 기존 행이 너무 긴 경우에는 SQL 내 위치가 없다.
    if (err->code == DB_ERR_IO || err->code == DB_ERR_SCHEMA
        || (err->code == DB_ERR_TOO_LONG && err->pos < 0)) {
        fprintf(stderr, "%s\n", err->message);
        return send_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "database operation failed");
    }
    return send_error(connection, MHD_HTTP_BAD_REQUEST, err->message);
}

static enum MHD_Result run_query(Db *db, struct MHD_Connection *connection, const Request *request)
{
    json_t *input = json_loadb(request->body, request->length, JSON_REJECT_DUPLICATES, NULL);
    if (input == NULL) {
        return send_error(connection, MHD_HTTP_BAD_REQUEST, "invalid JSON");
    }
    const char *sql = json_string_value(json_object_get(input, "sql"));
    if (sql == NULL) {
        json_decref(input);
        return send_error(connection, MHD_HTTP_BAD_REQUEST, "sql must be a string");
    }

    QueryRows collected = { .rows = json_array() };
    if (collected.rows == NULL) {
        json_decref(input);
        return send_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "could not collect results");
    }
    StmtType type;
    ExecResult result;
    DbError err;
    // sql 은 input 에서 빌린 문자열이다. 실행이 끝날 때까지 input 을 유지한다.
    DbStatus status = run_one_sql(db, sql, collect_row, &collected, &type, &result, &err);
    json_decref(input);

    enum MHD_Result sent;
    if (status != DB_OK) {
        sent = send_db_error(connection, &err);
    } else if (collected.failed) {
        sent = send_error(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, "could not collect results");
    } else if (type == STMT_INSERT) {
        sent = send_json(connection, MHD_HTTP_OK,
                         json_pack("{s:I}", "inserted_id", (json_int_t)result.inserted_id));
    } else {
        sent = send_json(connection, MHD_HTTP_OK,
                         json_pack("{s:O,s:i}", "rows", collected.rows, "row_count", result.row_count));
    }
    json_decref(collected.rows);
    return sent;
}

enum MHD_Result handle_request(void *db, struct MHD_Connection *connection,
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
    return run_query(db, connection, request);
}

void finish_request(void *cls, struct MHD_Connection *connection,
                    void **request_state, enum MHD_RequestTerminationCode reason)
{
    (void)cls;
    (void)connection;
    (void)reason;
    free(*request_state);
    *request_state = NULL;
}
