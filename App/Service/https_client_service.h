#ifndef HTTPS_CLIENT_SERVICE_H
#define HTTPS_CLIENT_SERVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Called from defaultTask poll loop.  The service manages DNS resolution,
   TLS handshake with CA verification, HTTP POST to the provisioning
   endpoint, and JSON response parsing.  All network operations are
   dispatched through tcpip_try_callback. */
void https_client_service_poll(void);

/* Returns 1 while a provisioning cycle is in progress (DNS / connect /
   request / response), 0 when idle. */
uint8_t https_client_service_is_busy(void);

/* Trigger an immediate provisioning cycle (resets retry timer). */
void https_client_service_trigger(void);

#ifdef __cplusplus
}
#endif

#endif /* HTTPS_CLIENT_SERVICE_H */
