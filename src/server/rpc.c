#include "rpc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"
#include "query.h"

// 요청 객체에서 꺼낸 것들. 문자열은 malloc 한 사본, params·id 는 본문 안의 위치(span)만 기억한다.
typedef struct {
    char       *jsonrpc;      // "2.0" 이어야 한다
    char       *method;       // "query", "ping"
    const char *params;       // params 값의 시작 위치 (없으면 NULL)
    const char *params_end;
    const char *id;           // id 값 원문의 시작 위치 (응답에 그대로 다시 쓴다). 없으면 NULL = 알림
    size_t      id_len;
} RpcRequest;

// [역할] RpcRequest 가 malloc 한 문자열을 놓는다.
static void request_free(RpcRequest *req)
{
    free(req->jsonrpc);
    free(req->method);
}

// [역할] 응답에 공통으로 붙는 꼬리 ,"id":... } 를 붙인다. id 가 없으면 null.
static void add_id_and_close(JsonBuf *b, const char *id, size_t id_len)
{
    jb_str(b, ",\"id\":");
    if (id != NULL) jb_add(b, id, id_len);   // 요청의 id 원문(숫자든 문자열이든)을 그대로
    else jb_str(b, "null");
    jb_str(b, "}");
}

// [역할] 성공 응답 {"jsonrpc":"2.0","result":<result_json>,"id":...} 을 만든다.
static char *make_result(const RpcRequest *req, const char *result_json)
{
    JsonBuf b = {0};
    jb_str(&b, "{\"jsonrpc\":\"2.0\",\"result\":");
    jb_str(&b, result_json);
    add_id_and_close(&b, req->id, req->id_len);
    return jb_finish(&b);
}

// [역할] 오류 응답 {"jsonrpc":"2.0","error":{"code":..,"message":..,"data":..},"id":...} 을 만든다.
// [인자] executed 가 0 이상이면 data 에 {"executed":N} 을 넣는다 (SQL 오류일 때만). -1 이면 data 없음.
static char *make_error(const char *id, size_t id_len, int code, const char *message, int executed)
{
    JsonBuf b = {0};
    jb_str(&b, "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":");
    jb_int(&b, code);
    jb_str(&b, ",\"message\":");
    jb_json_str(&b, message);
    if (executed >= 0) {
        jb_str(&b, ",\"data\":{\"executed\":");
        jb_int(&b, executed);
        jb_str(&b, "}");
    }
    jb_str(&b, "}");
    add_id_and_close(&b, id, id_len);
    return jb_finish(&b);
}

// [역할] 요청 객체 { ... } 를 훑어 jsonrpc / method / params / id 를 꺼낸다. 모르는 키는 건너뛴다.
// [반환] 0 = 성공, RPC_INVALID_REQUEST = 요청 모양이 아님
// [전제] 본문 전체가 올바른 JSON 이라는 것은 이미 확인했다 (그래서 여기서는 모양만 본다).
static int parse_request(JsonReader *r, RpcRequest *req)
{
    if (!jr_eat(r, '{')) return RPC_INVALID_REQUEST;   // 배열·숫자 등 객체가 아닌 요청
    if (jr_eat(r, '}')) return RPC_INVALID_REQUEST;    // 빈 객체

    do {
        char *key;
        if (jr_read_string(r, &key) != 0) return RPC_INVALID_REQUEST;
        jr_eat(r, ':');
        jr_skip_ws(r);
        const char *value = r->p;
        int is_string = (*value == '"');

        if (strcmp(key, "jsonrpc") == 0 && is_string) {
            free(req->jsonrpc);
            jr_read_string(r, &req->jsonrpc);
        } else if (strcmp(key, "method") == 0 && is_string) {
            free(req->method);
            jr_read_string(r, &req->method);
        } else {
            jr_skip_value(r);
            if (strcmp(key, "params") == 0) {
                req->params = value;
                req->params_end = r->p;
            } else if (strcmp(key, "id") == 0) {
                // 규격: id 는 문자열·숫자·null 만. 객체·배열·true/false 는 틀린 요청
                if (*value == '{' || *value == '[' || *value == 't' || *value == 'f') {
                    free(key);
                    return RPC_INVALID_REQUEST;
                }
                req->id = value;
                req->id_len = (size_t)(r->p - value);
            }
        }
        free(key);
    } while (jr_eat(r, ','));

    if (req->jsonrpc == NULL || strcmp(req->jsonrpc, "2.0") != 0) return RPC_INVALID_REQUEST;
    if (req->method == NULL) return RPC_INVALID_REQUEST;
    return 0;
}

// [역할] query 메서드의 params 에서 SQL 문자열을 꺼낸다.
// [기능] 이름 방식 {"sql":"..."} 과 위치 방식 ["..."] 둘 다 받는다.
// [반환] 0 = 성공 (*sql 은 부른 쪽이 free), -1 = sql 이 없거나 문자열이 아님
static int get_sql_param(const RpcRequest *req, char **sql)
{
    if (req->params == NULL) return -1;
    JsonReader r = { req->params, req->params_end };

    if (jr_eat(&r, '[')) {                          // ["SELECT ..."]
        jr_skip_ws(&r);
        return (r.p < r.end && *r.p == '"') ? jr_read_string(&r, sql) : -1;
    }
    if (!jr_eat(&r, '{') || jr_eat(&r, '}')) return -1;
    do {                                            // {"sql":"SELECT ..."}
        char *key;
        if (jr_read_string(&r, &key) != 0) return -1;
        jr_eat(&r, ':');
        jr_skip_ws(&r);
        int found = strcmp(key, "sql") == 0;
        free(key);
        if (found) return (r.p < r.end && *r.p == '"') ? jr_read_string(&r, sql) : -1;
        jr_skip_value(&r);
    } while (jr_eat(&r, ','));
    return -1;
}

// [역할] 메서드 이름으로 실제 일을 하고 응답 JSON 을 만든다 (메서드 디스패치).
// [기능] query → query_run, ping → "pong", 그 밖 → -32601
static char *call_method(Db *db, const RpcRequest *req)
{
    if (strcmp(req->method, "ping") == 0) {
        return make_result(req, "\"pong\"");
    }
    if (strcmp(req->method, "query") == 0) {
        char *sql = NULL;
        if (get_sql_param(req, &sql) != 0) {
            return make_error(req->id, req->id_len, RPC_INVALID_PARAMS,
                              "params must be {\"sql\": \"...\"} or [\"...\"]", -1);
        }
        QueryError qe;
        char *result = query_run(db, sql, &qe);
        free(sql);
        if (result == NULL) {
            // SQL 오류만 data.executed 를 붙인다 (몇 문장이 이미 반영됐는지 알려 주려고)
            int with_data = (qe.code != RPC_INVALID_PARAMS && qe.code != RPC_INTERNAL_ERROR);
            return make_error(req->id, req->id_len, qe.code, qe.message, with_data ? qe.executed : -1);
        }
        char *resp = make_result(req, result);
        free(result);
        return resp;
    }
    char msg[160];
    snprintf(msg, sizeof msg, "method '%.100s' not found", req->method);
    return make_error(req->id, req->id_len, RPC_METHOD_NOT_FOUND, msg, -1);
}

char *rpc_handle(Db *db, const char *body, size_t len, int *notification)
{
    RpcRequest req = {0};
    *notification = 0;

    // 1) 본문 전체가 올바른 JSON 인지 먼저 본다. 아니면 -32700 (id 를 알 수 없으니 null).
    JsonReader check = { body, body + len };
    if (jr_skip_value(&check) != 0 || (jr_skip_ws(&check), check.p != check.end)) {
        return make_error(NULL, 0, RPC_PARSE_ERROR, "parse error: body is not valid JSON", -1);
    }

    // 2) 배치 요청 [ {...}, {...} ] 은 지원하지 않는다. (한 요청의 sql 에 여러 문장을 넣으면 된다)
    JsonReader r = { body, body + len };
    jr_skip_ws(&r);
    if (r.p < r.end && *r.p == '[') {
        return make_error(NULL, 0, RPC_INVALID_REQUEST, "batch requests are not supported", -1);
    }

    // 3) 요청 객체 해석. 모양이 틀리면 -32600.
    if (parse_request(&r, &req) != 0) {
        char *resp = make_error(req.id, req.id_len, RPC_INVALID_REQUEST,
                                "invalid request: need \"jsonrpc\":\"2.0\" and \"method\"", -1);
        request_free(&req);
        return resp;
    }

    // 4) 메서드 실행. id 가 없는 알림이면 실행만 하고 응답은 버린다.
    char *resp = call_method(db, &req);
    if (req.id == NULL) {
        free(resp);
        resp = NULL;
        *notification = 1;
    }
    request_free(&req);
    return resp;
}
