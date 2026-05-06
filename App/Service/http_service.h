#ifndef HTTP_SERVICE_H
#define HTTP_SERVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
  uint32_t connections;
  uint32_t requests;
  uint32_t errors;
  uint32_t active_connections;
  uint32_t close_pending_connections;
  uint32_t listener_up;
} http_service_stats_t;

void http_service_init(void);
void http_service_poll(void);
void http_service_get_stats(http_service_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif /* HTTP_SERVICE_H */
