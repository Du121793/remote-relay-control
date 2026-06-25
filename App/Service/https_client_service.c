#include "https_client_service.h"

#include "cluster_service.h"
#include "mqtt_link_service.h"
#include "../Config/device_config.h"

#include "FreeRTOS.h"
#include "task.h"

#include "lwip/altcp.h"
#include "lwip/altcp_tcp.h"
#include "lwip/altcp_tls.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"

#include <stdio.h>
#include <string.h>

extern struct netif gnetif;

#define HTTPS_CLIENT_START_DELAY_MS      3000UL
#define HTTPS_CLIENT_PROVISION_HOST      "autocontrol.du1217.club"
#define HTTPS_CLIENT_PROVISION_PORT      443U
#define HTTPS_CLIENT_PROVISION_PATH      "/api/provision"
#define HTTPS_CLIENT_CONNECT_TIMEOUT_MS  15000UL
#define HTTPS_CLIENT_RECV_TIMEOUT_MS     10000UL
#define HTTPS_CLIENT_RX_BUFFER_SIZE      2048U
#define HTTPS_CLIENT_RETRY_INTERVAL_MS   (5UL * 60UL * 1000UL)

typedef enum
{
  HTTPS_STATE_IDLE = 0,
  HTTPS_STATE_DNS,
  HTTPS_STATE_CONNECTING,
  HTTPS_STATE_CONNECTED,
  HTTPS_STATE_SENDING,
  HTTPS_STATE_RECEIVING,
  HTTPS_STATE_DONE
} https_client_state_t;

typedef struct
{
  struct altcp_pcb *pcb;
  ip_addr_t server_addr;
  uint8_t state;
  volatile uint8_t callback_pending;
  volatile uint8_t restart_requested;
  uint32_t deadline_ms;
  uint32_t next_attempt_ms;
  char rx_buffer[HTTPS_CLIENT_RX_BUFFER_SIZE];
  size_t rx_used;
} https_client_context_t;

static https_client_context_t g_https_client_context;

static void https_client_tcpip_step(void *argument);
static void https_client_dns_callback(const char *name,
                                      const ip_addr_t *ipaddr,
                                      void *argument);
static err_t https_client_connected(void *arg, struct altcp_pcb *pcb, err_t err);
static err_t https_client_recv(void *arg, struct altcp_pcb *pcb,
                               struct pbuf *p, err_t err);
static void https_client_err(void *arg, err_t err);

static uint8_t https_client_is_master_online(void)
{
  cluster_service_state_t cluster_state;

  cluster_service_get_state(&cluster_state);
  return ((cluster_state.role == CLUSTER_ROLE_MASTER) &&
          (netif_is_up(&gnetif) != 0) &&
          (netif_is_link_up(&gnetif) != 0) &&
          (ip4_addr_isany_val(*netif_ip4_addr(&gnetif)) == 0))
         ? 1U
         : 0U;
}

static void https_client_close(void)
{
  if (g_https_client_context.pcb != NULL)
  {
    altcp_abort(g_https_client_context.pcb);
    g_https_client_context.pcb = NULL;
  }
  g_https_client_context.state = HTTPS_STATE_IDLE;
}

static err_t https_client_connected(void *arg, struct altcp_pcb *pcb, err_t err)
{
  (void)arg;

  if ((err != ERR_OK) || (g_https_client_context.state != HTTPS_STATE_CONNECTING))
  {
    https_client_close();
    return ERR_OK;
  }

  g_https_client_context.state = HTTPS_STATE_CONNECTED;
  g_https_client_context.rx_used = 0U;
  return ERR_OK;
}

static void https_client_send_request(void)
{
  char request[1024];
  char fingerprint_json[128];
  const device_config_t *config;
  int req_len;
  err_t err;

  config = device_config_get();
  if (config == NULL)
  {
    https_client_close();
    return;
  }

  /* JSON-escape the device fingerprint directly in the snprintf */
  fingerprint_json[0] = '\0';
  /* We send just the raw fingerprint hex — the backend compares it. */
  {
    size_t fp_len = strlen(config->cloud_cluster_ca_fingerprint);
    if (fp_len > 0U && fp_len < (sizeof(fingerprint_json) - 4U))
    {
      fingerprint_json[0] = '"';
      memcpy(&fingerprint_json[1], config->cloud_cluster_ca_fingerprint, fp_len);
      fingerprint_json[fp_len + 1U] = '"';
      fingerprint_json[fp_len + 2U] = '\0';
    }
    else
    {
      memcpy(fingerprint_json, "\"\"", 3);
    }
  }

  req_len = snprintf(request, sizeof(request),
                     "POST %s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %u\r\n"
                     "Connection: close\r\n"
                     "\r\n"
                     "{\"cluster_id\":\"%s\",\"device_id\":\"%s\",\"device_fingerprint\":%s}\n",
                     HTTPS_CLIENT_PROVISION_PATH,
                     HTTPS_CLIENT_PROVISION_HOST,
                     (unsigned int)(strlen(config->cloud_cluster_id) +
                                    strlen(config->cloud_device_id) +
                                    strlen(config->cloud_cluster_ca_fingerprint) + 64U),
                     config->cloud_cluster_id,
                     config->cloud_device_id,
                     fingerprint_json);
  if ((req_len <= 0) || ((size_t)req_len >= sizeof(request)))
  {
    https_client_close();
    return;
  }

  err = altcp_write(g_https_client_context.pcb, request, (u16_t)req_len,
                    TCP_WRITE_FLAG_COPY);
  if (err != ERR_OK)
  {
    https_client_close();
    return;
  }

  (void)altcp_output(g_https_client_context.pcb);
  g_https_client_context.state = HTTPS_STATE_SENDING;
}

static void https_client_parse_and_apply(const char *body, size_t body_len)
{
  const device_config_t *config = device_config_get();
  static device_config_t updated;
  static char value_buffer[DEVICE_CONFIG_CA_CERT_MAX_LEN];
  const char *ptr;

  if ((config == NULL) || (body == NULL) || (body_len < 8U))
  {
    return;
  }

  /* Quick check for JSON ok:true */
  if (strstr(body, "\"ok\":true") == NULL)
  {
    return;
  }

  updated = *config;

  /* Extract cloud_key_id — the backend assigns each device a unique key
     that serves as the MQTT username.  The MCU must store and use this
     value so the broker's registry lookup succeeds. */
  ptr = strstr(body, "\"cloud_key_id\":\"");
  if (ptr != NULL)
  {
    const char *start = ptr + 16;
    const char *end = strchr(start, '"');
    size_t len;

    if ((end != NULL) && (end > start) &&
        ((len = (size_t)(end - start)) < sizeof(updated.cloud_key_id)))
    {
      memset(updated.cloud_key_id, 0, sizeof(updated.cloud_key_id));
      memcpy(updated.cloud_key_id, start, len);
    }
  }

  /* Extract binding_secret */
  ptr = strstr(body, "\"cloud_cluster_binding_secret\":\"");
  if (ptr != NULL)
  {
    const char *start = ptr + 35;
    const char *end = strchr(start, '"');
    size_t len;

    if ((end != NULL) && (end > start) &&
        ((len = (size_t)(end - start)) < sizeof(updated.cloud_cluster_binding_secret)))
    {
      memset(updated.cloud_cluster_binding_secret, 0,
             sizeof(updated.cloud_cluster_binding_secret));
      memcpy(updated.cloud_cluster_binding_secret, start, len);
    }
  }

  /* Extract CA cert PEM */
  ptr = strstr(body, "\"cloud_cluster_ca_cert_pem\":\"");
  if (ptr != NULL)
  {
    const char *start = ptr + 28;
    char *dest = value_buffer;
    size_t dest_len = 0U;

    memset(value_buffer, 0, DEVICE_CONFIG_CA_CERT_MAX_LEN);
    while ((*start != '\0') && (dest_len < (DEVICE_CONFIG_CA_CERT_MAX_LEN - 1U)))
    {
      if ((start[0] == '\\') && (start[1] == 'n'))
      {
        dest[dest_len++] = '\n';
        start += 2;
      }
      else if ((start[0] == '\\') && (start[1] == '"'))
      {
        /* End of JSON string */
        break;
      }
      else if (start[0] == '\\')
      {
        dest[dest_len++] = *start++;
      }
      else
      {
        dest[dest_len++] = *start++;
      }
    }
    if (dest_len > 0U && dest_len < sizeof(updated.cloud_cluster_ca_cert))
    {
      memset(updated.cloud_cluster_ca_cert, 0,
             sizeof(updated.cloud_cluster_ca_cert));
      memcpy(updated.cloud_cluster_ca_cert, value_buffer, dest_len);
    }
  }

  /* Extract provisioning_revision — when this increments the MCU
     re-provisions automatically and rotates its PSK / CA material. */
  {
    const char *rev_ptr = strstr(body, "\"provisioning_revision\":");
    if (rev_ptr != NULL)
    {
      uint32_t rev = 0U;
      const char *num = rev_ptr + 24;
      while (*num >= '0' && *num <= '9') { rev = rev * 10U + (uint32_t)(*num - '0'); num++; }
      updated.provisioning_revision = rev;
    }
  }
  /* Extract ca_revocation_tag — the backend publishes this when a
     CA certificate has been revoked.  The MQTT client checks it
     before every connection. */
  {
    const char *tag_ptr = strstr(body, "\"ca_revocation_tag\":");
    if (tag_ptr != NULL)
    {
      uint32_t tag = 0U;
      const char *num = tag_ptr + 20;
      while (*num >= '0' && *num <= '9') { tag = tag * 10U + (uint32_t)(*num - '0'); num++; }
      updated.ca_revocation_tag = tag;
    }
  }

  if (updated.provisioning_revision != config->provisioning_revision ||
      updated.ca_revocation_tag != config->ca_revocation_tag ||
      strcmp(updated.cloud_key_id, config->cloud_key_id) != 0 ||
      strcmp(updated.cloud_cluster_binding_secret, config->cloud_cluster_binding_secret) != 0)
  {
    (void)device_config_update_and_save(&updated);
    mqtt_link_service_mark_status_dirty();
  }
}

static err_t https_client_recv(void *arg, struct altcp_pcb *pcb,
                               struct pbuf *p, err_t err)
{
  (void)arg;
  (void)pcb;

  if (p == NULL)
  {
    /* Connection closed by server */
    if (g_https_client_context.rx_used > 0U)
    {
      g_https_client_context.rx_buffer[g_https_client_context.rx_used] = '\0';
      https_client_parse_and_apply(g_https_client_context.rx_buffer,
                                   g_https_client_context.rx_used);
    }
    https_client_close();
    g_https_client_context.next_attempt_ms =
      HAL_GetTick() + HTTPS_CLIENT_RETRY_INTERVAL_MS;
    return ERR_OK;
  }

  if (err != ERR_OK)
  {
    pbuf_free(p);
    https_client_close();
    return ERR_OK;
  }

  if (g_https_client_context.state == HTTPS_STATE_SENDING)
  {
    g_https_client_context.state = HTTPS_STATE_RECEIVING;
  }

  /* Accumulate response body (skip HTTP headers in production — we just
     buffer everything and the JSON parser finds the { ... } block). */
  {
    size_t copy_len = (size_t)p->tot_len;
    if ((g_https_client_context.rx_used + copy_len) <
        sizeof(g_https_client_context.rx_buffer))
    {
      pbuf_copy_partial(p,
                        &g_https_client_context.rx_buffer[g_https_client_context.rx_used],
                        (u16_t)copy_len, 0U);
      g_https_client_context.rx_used += copy_len;
    }
  }

  { u16_t rlen = (u16_t)p->tot_len; altcp_recved(pcb, rlen); } pbuf_free(p);

  return ERR_OK;
}

static void https_client_err(void *arg, err_t err)
{
  (void)arg;
  (void)err;
  /* lwIP error callback fires AFTER the PCB is already freed.
     Do NOT call altcp_abort() — just clear our reference. */
  g_https_client_context.pcb = NULL;
  g_https_client_context.state = HTTPS_STATE_IDLE;
}

static void https_client_connect_to(const ip_addr_t *address)
{
  err_t err;
  const device_config_t *config = device_config_get();

  if ((address == NULL) || (config == NULL))
  {
    return;
  }

  {
    const u8_t *ca_cert = (const u8_t *)config->cloud_cluster_ca_cert;
    size_t ca_cert_len = (config->cloud_cluster_ca_cert[0] != '\0')
                         ? strlen(config->cloud_cluster_ca_cert)
                         : 0U;

    if (ca_cert_len > 0U)
    {
      g_https_client_context.pcb =
        altcp_tls_new(altcp_tls_create_config_client(ca_cert, ca_cert_len),
                      IPADDR_TYPE_V4);
    }
    else
    {
      /* No CA cert configured — TLS without server verification.
         The connection is still encrypted but vulnerable to MITM. */
      g_https_client_context.pcb =
        altcp_tls_new(altcp_tls_create_config_client(NULL, 0U),
                      IPADDR_TYPE_V4);
    }
  }
  if (g_https_client_context.pcb == NULL)
  {
    return;
  }

  altcp_arg(g_https_client_context.pcb, NULL);
  altcp_err(g_https_client_context.pcb, https_client_err);
  altcp_recv(g_https_client_context.pcb, https_client_recv);

  err = altcp_connect(g_https_client_context.pcb,
                      address,
                      HTTPS_CLIENT_PROVISION_PORT,
                      https_client_connected);
  if (err != ERR_OK)
  {
    altcp_abort(g_https_client_context.pcb);
    g_https_client_context.pcb = NULL;
    return;
  }

  g_https_client_context.state = HTTPS_STATE_CONNECTING;
  g_https_client_context.deadline_ms =
    HAL_GetTick() + HTTPS_CLIENT_CONNECT_TIMEOUT_MS;
}

static void https_client_dns_callback(const char *name,
                                      const ip_addr_t *ipaddr,
                                      void *argument)
{
  (void)name;
  (void)argument;

  if (g_https_client_context.state != HTTPS_STATE_DNS)
  {
    return;
  }
  if ((ipaddr == NULL) || (IP_IS_V4(ipaddr) == 0))
  {
    g_https_client_context.next_attempt_ms =
      HAL_GetTick() + HTTPS_CLIENT_RETRY_INTERVAL_MS;
    g_https_client_context.state = HTTPS_STATE_IDLE;
    return;
  }
  https_client_connect_to(ipaddr);
}

static void https_client_start(void)
{
  ip_addr_t address;
  err_t dns_err;

  g_https_client_context.state = HTTPS_STATE_DNS;
  g_https_client_context.deadline_ms =
    HAL_GetTick() + HTTPS_CLIENT_CONNECT_TIMEOUT_MS;
  g_https_client_context.rx_used = 0U;

  dns_err = dns_gethostbyname(HTTPS_CLIENT_PROVISION_HOST,
                              &address,
                              https_client_dns_callback,
                              NULL);
  if (dns_err == ERR_OK)
  {
    https_client_connect_to(&address);
  }
  else if (dns_err != ERR_INPROGRESS)
  {
    g_https_client_context.next_attempt_ms =
      HAL_GetTick() + HTTPS_CLIENT_RETRY_INTERVAL_MS;
    g_https_client_context.state = HTTPS_STATE_IDLE;
  }
}

static void https_client_tcpip_step(void *argument)
{
  const uint32_t now = HAL_GetTick();

  (void)argument;
  g_https_client_context.callback_pending = 0U;

  /* Process restart request from non-tcpip threads */
  if (g_https_client_context.restart_requested != 0U)
  {
    g_https_client_context.restart_requested = 0U;
    if (g_https_client_context.pcb != NULL) { https_client_close(); }
    g_https_client_context.state = HTTPS_STATE_IDLE;
  }

  if (https_client_is_master_online() == 0U)
  {
    https_client_close();
    g_https_client_context.next_attempt_ms = 0U;
    return;
  }

  {
    const device_config_t *config = device_config_get();

    if ((config == NULL) ||
        (config->cloud_enabled == 0U) ||
        (config->cloud_cluster_id[0] == '\0') ||
        (config->cloud_device_id[0] == '\0'))
    {
      return;
    }

    /* Provisioning only runs once after config is populated.
       It re-runs every RETRY_INTERVAL_MS if the binding secret
       is still empty. */
    if (config->cloud_cluster_binding_secret[0] != '\0')
    {
      return;
    }
  }

  if ((g_https_client_context.state == HTTPS_STATE_DNS) ||
      (g_https_client_context.state == HTTPS_STATE_CONNECTING))
  {
    if ((int32_t)(now - g_https_client_context.deadline_ms) >= 0)
    {
      https_client_close();
      g_https_client_context.next_attempt_ms =
        now + HTTPS_CLIENT_RETRY_INTERVAL_MS;
    }
    return;
  }

  if ((g_https_client_context.state == HTTPS_STATE_CONNECTED) ||
      (g_https_client_context.state == HTTPS_STATE_SENDING))
  {
    if (g_https_client_context.state == HTTPS_STATE_CONNECTED)
    {
      https_client_send_request();
    }
    return;
  }

  if (g_https_client_context.state == HTTPS_STATE_IDLE)
  {
    if (g_https_client_context.next_attempt_ms == 0U)
    {
      g_https_client_context.next_attempt_ms =
        now + HTTPS_CLIENT_START_DELAY_MS;
      return;
    }
    if ((int32_t)(now - g_https_client_context.next_attempt_ms) >= 0)
    {
      https_client_start();
    }
  }
}

void https_client_service_poll(void)
{
  if (g_https_client_context.callback_pending != 0U)
  {
    return;
  }

  g_https_client_context.callback_pending = 1U;
  if (tcpip_try_callback(https_client_tcpip_step, NULL) != ERR_OK)
  {
    g_https_client_context.callback_pending = 0U;
  }
}

uint8_t https_client_service_is_busy(void)
{
  return (g_https_client_context.state != HTTPS_STATE_IDLE) ? 1U : 0U;
}

void https_client_service_trigger(void)
{
  /* Only set flags — tcpip_step handles actual close/restart in lwIP context.
     This avoids racing on PCB ownership from non-tcpip callers. */
  g_https_client_context.restart_requested = 1U;
  g_https_client_context.next_attempt_ms = 0U;
}
