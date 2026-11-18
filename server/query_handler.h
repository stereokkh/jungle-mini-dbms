#ifndef QUERY_HANDLER_H
#define QUERY_HANDLER_H

#include <microhttpd.h>

#include "db.h"

typedef struct QueryPool QueryPool;

QueryPool *query_pool_start(Db *db);
// 새 작업을 거절하고, 받아 둔 요청이 완료될 때까지 HTTP 데몬을 유지한다.
void query_pool_drain(QueryPool *pool);
// 워커와 HTTP 데몬을 모두 종료한 뒤 호출한다.
void query_pool_destroy(QueryPool *pool);

enum MHD_Result handle_request(void *cls, struct MHD_Connection *connection,
                              const char *url, const char *method, const char *version,
                              const char *upload_data, size_t *upload_size, void **request_state);

void finish_request(void *cls, struct MHD_Connection *connection,
                    void **request_state, enum MHD_RequestTerminationCode reason);

#endif
