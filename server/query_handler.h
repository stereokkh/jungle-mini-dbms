#ifndef QUERY_HANDLER_H
#define QUERY_HANDLER_H

#include <microhttpd.h>

enum MHD_Result handle_request(void *db, struct MHD_Connection *connection,
                              const char *url, const char *method, const char *version,
                              const char *upload_data, size_t *upload_size, void **request_state);

void finish_request(void *cls, struct MHD_Connection *connection,
                    void **request_state, enum MHD_RequestTerminationCode reason);

#endif
