#include "time_sync_service.h"

#include "cluster_service.h"

#include "FreeRTOS.h"
#include "task.h"

#include "lwip/dns.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "lwip/udp.h"

#include <string.h>

extern struct netif gnetif;

#define TIME_SYNC_NTP_HOST                    "pool.ntp.org"
#define TIME_SYNC_NTP_PORT                    123U
#define TIME_SYNC_NTP_PACKET_SIZE             48U
#define TIME_SYNC_NTP_TO_UNIX_SECONDS         2208988800UL
#define TIME_SYNC_MASTER_STABLE_MS             7000UL
#define TIME_SYNC_START_DELAY_MS              1000UL
#define TIME_SYNC_NTP_TIMEOUT_MS              5000UL
#define TIME_SYNC_BACKEND_TIMEOUT_MS          10000UL
#define TIME_SYNC_SUCCESS_INTERVAL_MS         (6UL * 60UL * 60UL * 1000UL)
#define TIME_SYNC_BACKEND_RETRY_NTP_MS        (15UL * 60UL * 1000UL)
#define TIME_SYNC_FAILURE_RETRY_MS            30000UL
#define TIME_SYNC_MIN_UNIX                    1577836800UL
#define TIME_SYNC_MAX_UNIX                    4102444799UL

#define TIME_SYNC_ERROR_NONE                  0U
#define TIME_SYNC_ERROR_DNS                   1U
#define TIME_SYNC_ERROR_UDP_ALLOC             2U
#define TIME_SYNC_ERROR_PBUF                  3U
#define TIME_SYNC_ERROR_SEND                  4U
#define TIME_SYNC_ERROR_TIMEOUT               5U
#define TIME_SYNC_ERROR_RESPONSE              6U
#define TIME_SYNC_ERROR_BACKEND_TIMEOUT       7U
#define TIME_SYNC_ERROR_BACKEND_VALUE         8U

typedef enum
{
  TIME_SYNC_STATE_IDLE = 0,
  TIME_SYNC_STATE_DNS,
  TIME_SYNC_STATE_NTP_WAIT,
  TIME_SYNC_STATE_BACKEND_WAIT
} time_sync_state_t;

typedef struct
{
  struct udp_pcb *pcb;
  ip_addr_t server_addr;
  uint8_t request_marker[8];
  volatile uint8_t callback_pending;
  volatile uint8_t state;
  uint32_t master_since_ms;
  uint32_t deadline_ms;
  uint32_t next_attempt_ms;
  time_sync_service_stats_t stats;
} time_sync_service_context_t;

static time_sync_service_context_t g_time_sync_context;

static void time_sync_tcpip_step(void *argument);
static void time_sync_dns_callback(const char *name,
                                   const ip_addr_t *ipaddr,
                                   void *argument);
static void time_sync_udp_receive(void *argument,
                                  struct udp_pcb *pcb,
                                  struct pbuf *packet,
                                  const ip_addr_t *address,
                                  u16_t port);

static uint32_t time_sync_read_be32(const uint8_t *data)
{
  return ((uint32_t)data[0] << 24U) |
         ((uint32_t)data[1] << 16U) |
         ((uint32_t)data[2] << 8U) |
         (uint32_t)data[3];
}

static uint8_t time_sync_is_master_online(void)
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

static void time_sync_close_udp(void)
{
  if (g_time_sync_context.pcb != NULL)
  {
    udp_remove(g_time_sync_context.pcb);
    g_time_sync_context.pcb = NULL;
  }
}

static void time_sync_schedule(uint32_t delay_ms)
{
  /* MQTT time sync is now the primary path; backend TCP retired */
  g_time_sync_context.state = TIME_SYNC_STATE_IDLE;
  g_time_sync_context.deadline_ms = 0U;
  g_time_sync_context.next_attempt_ms = HAL_GetTick() + delay_ms;
  g_time_sync_context.stats.active = 0U;
  g_time_sync_context.stats.waiting_backend = 0U;
  g_time_sync_context.stats.next_attempt_ms =
    g_time_sync_context.next_attempt_ms;
  time_sync_close_udp();
}

static void time_sync_start_backend_fallback(uint32_t error)
{
  const uint32_t now = HAL_GetTick();

  time_sync_close_udp();
  g_time_sync_context.stats.ntp_failures++;
  g_time_sync_context.stats.last_error = error;
  g_time_sync_context.stats.backend_requests++;
  g_time_sync_context.stats.active = 1U;
  g_time_sync_context.stats.waiting_backend = 1U;
  g_time_sync_context.state = TIME_SYNC_STATE_BACKEND_WAIT;
  g_time_sync_context.deadline_ms = now + TIME_SYNC_BACKEND_TIMEOUT_MS;
  /* MQTT time sync is now the primary path */
  /* backend_link_service_request_time() has been retired */
}

static void time_sync_send_request(const ip_addr_t *address)
{
  struct pbuf *packet;
  uint8_t payload[TIME_SYNC_NTP_PACKET_SIZE];
  uint8_t time_valid;
  uint32_t marker;
  uint32_t unix_time;
  uint32_t ntp_seconds;
  err_t error;

  if ((address == NULL) || (IP_IS_V4(address) == 0))
  {
    time_sync_start_backend_fallback(TIME_SYNC_ERROR_DNS);
    return;
  }

  if (g_time_sync_context.pcb == NULL)
  {
    g_time_sync_context.pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (g_time_sync_context.pcb == NULL)
    {
      time_sync_start_backend_fallback(TIME_SYNC_ERROR_UDP_ALLOC);
      return;
    }
    udp_recv(g_time_sync_context.pcb, time_sync_udp_receive, NULL);
  }

  memset(payload, 0, sizeof(payload));
  payload[0] = 0x23U;
  unix_time = cluster_service_get_unix_time(&time_valid);
  ntp_seconds = (time_valid != 0U)
                ? unix_time + TIME_SYNC_NTP_TO_UNIX_SECONDS
                : 0U;
  payload[40] = (uint8_t)(ntp_seconds >> 24U);
  payload[41] = (uint8_t)(ntp_seconds >> 16U);
  payload[42] = (uint8_t)(ntp_seconds >> 8U);
  payload[43] = (uint8_t)ntp_seconds;
  marker = HAL_GetTick() ^ 0xA5C35A7EUL;
  payload[44] = (uint8_t)(marker >> 24U);
  payload[45] = (uint8_t)(marker >> 16U);
  payload[46] = (uint8_t)(marker >> 8U);
  payload[47] = (uint8_t)marker;
  memcpy(g_time_sync_context.request_marker, &payload[40], 8U);
  ip_addr_copy(g_time_sync_context.server_addr, *address);

  packet = pbuf_alloc(PBUF_TRANSPORT, sizeof(payload), PBUF_RAM);
  if (packet == NULL)
  {
    time_sync_start_backend_fallback(TIME_SYNC_ERROR_PBUF);
    return;
  }
  error = pbuf_take(packet, payload, sizeof(payload));
  if (error == ERR_OK)
  {
    error = udp_sendto(g_time_sync_context.pcb,
                       packet,
                       address,
                       TIME_SYNC_NTP_PORT);
  }
  pbuf_free(packet);

  if (error != ERR_OK)
  {
    time_sync_start_backend_fallback(TIME_SYNC_ERROR_SEND);
    return;
  }

  g_time_sync_context.state = TIME_SYNC_STATE_NTP_WAIT;
  g_time_sync_context.deadline_ms = HAL_GetTick() + TIME_SYNC_NTP_TIMEOUT_MS;
}

static void time_sync_dns_callback(const char *name,
                                   const ip_addr_t *ipaddr,
                                   void *argument)
{
  (void)name;
  (void)argument;

  if (g_time_sync_context.state != TIME_SYNC_STATE_DNS)
  {
    return;
  }
  if (ipaddr == NULL)
  {
    time_sync_start_backend_fallback(TIME_SYNC_ERROR_DNS);
    return;
  }
  time_sync_send_request(ipaddr);
}

static void time_sync_udp_receive(void *argument,
                                  struct udp_pcb *pcb,
                                  struct pbuf *packet,
                                  const ip_addr_t *address,
                                  u16_t port)
{
  uint8_t payload[TIME_SYNC_NTP_PACKET_SIZE];
  uint32_t ntp_seconds;
  uint32_t unix_time;
  uint8_t mode;
  uint8_t stratum;

  (void)argument;
  (void)pcb;

  if (packet == NULL)
  {
    return;
  }

  if ((g_time_sync_context.state != TIME_SYNC_STATE_NTP_WAIT) ||
      (port != TIME_SYNC_NTP_PORT) ||
      (address == NULL) ||
      (ip_addr_cmp(address, &g_time_sync_context.server_addr) == 0) ||
      (packet->tot_len < TIME_SYNC_NTP_PACKET_SIZE) ||
      (pbuf_copy_partial(packet,
                         payload,
                         sizeof(payload),
                         0U) != sizeof(payload)))
  {
    pbuf_free(packet);
    return;
  }
  pbuf_free(packet);

  mode = payload[0] & 0x07U;
  stratum = payload[1];
  if ((mode != 4U) ||
      (stratum == 0U) ||
      (stratum >= 16U) ||
      (memcmp(&payload[24],
              g_time_sync_context.request_marker,
              sizeof(g_time_sync_context.request_marker)) != 0))
  {
    time_sync_start_backend_fallback(TIME_SYNC_ERROR_RESPONSE);
    return;
  }

  ntp_seconds = time_sync_read_be32(&payload[40]);
  if (ntp_seconds <= TIME_SYNC_NTP_TO_UNIX_SECONDS)
  {
    time_sync_start_backend_fallback(TIME_SYNC_ERROR_RESPONSE);
    return;
  }
  unix_time = ntp_seconds - TIME_SYNC_NTP_TO_UNIX_SECONDS;
  if ((unix_time < TIME_SYNC_MIN_UNIX) ||
      (unix_time > TIME_SYNC_MAX_UNIX) ||
      (cluster_service_set_unix_time(unix_time,
                                     CLUSTER_TIME_SOURCE_NTP) == 0U))
  {
    time_sync_start_backend_fallback(TIME_SYNC_ERROR_RESPONSE);
    return;
  }

  g_time_sync_context.stats.ntp_successes++;
  g_time_sync_context.stats.last_sync_ms = HAL_GetTick();
  g_time_sync_context.stats.last_source = CLUSTER_TIME_SOURCE_NTP;
  g_time_sync_context.stats.last_error = TIME_SYNC_ERROR_NONE;
  time_sync_schedule(TIME_SYNC_SUCCESS_INTERVAL_MS);
}

static void time_sync_start_ntp(void)
{
  ip_addr_t address;
  err_t error;

  g_time_sync_context.stats.ntp_attempts++;
  g_time_sync_context.stats.active = 1U;
  g_time_sync_context.stats.waiting_backend = 0U;
  g_time_sync_context.state = TIME_SYNC_STATE_DNS;
  g_time_sync_context.deadline_ms = HAL_GetTick() + TIME_SYNC_NTP_TIMEOUT_MS;

  error = dns_gethostbyname(TIME_SYNC_NTP_HOST,
                            &address,
                            time_sync_dns_callback,
                            NULL);
  if (error == ERR_OK)
  {
    time_sync_send_request(&address);
  }
  else if (error != ERR_INPROGRESS)
  {
    time_sync_start_backend_fallback(TIME_SYNC_ERROR_DNS);
  }
}

static void time_sync_tcpip_step(void *argument)
{
  const uint32_t now = HAL_GetTick();

  (void)argument;
  g_time_sync_context.callback_pending = 0U;

  if (time_sync_is_master_online() == 0U)
  {
    /* MQTT time sync is now the primary path; backend TCP retired */
    time_sync_close_udp();
    g_time_sync_context.state = TIME_SYNC_STATE_IDLE;
    g_time_sync_context.master_since_ms = 0U;
    g_time_sync_context.deadline_ms = 0U;
    g_time_sync_context.next_attempt_ms = 0U;
    g_time_sync_context.stats.active = 0U;
    g_time_sync_context.stats.waiting_backend = 0U;
    g_time_sync_context.stats.next_attempt_ms = 0U;
    return;
  }

  if (g_time_sync_context.master_since_ms == 0U)
  {
    g_time_sync_context.master_since_ms = now;
    return;
  }
  if ((int32_t)(now - g_time_sync_context.master_since_ms) <
      (int32_t)TIME_SYNC_MASTER_STABLE_MS)
  {
    return;
  }

  if (g_time_sync_context.next_attempt_ms == 0U)
  {
    g_time_sync_context.next_attempt_ms = now + TIME_SYNC_START_DELAY_MS;
    g_time_sync_context.stats.next_attempt_ms =
      g_time_sync_context.next_attempt_ms;
  }

  if (((g_time_sync_context.state == TIME_SYNC_STATE_DNS) ||
       (g_time_sync_context.state == TIME_SYNC_STATE_NTP_WAIT)) &&
      ((int32_t)(now - g_time_sync_context.deadline_ms) >= 0))
  {
    time_sync_start_backend_fallback(TIME_SYNC_ERROR_TIMEOUT);
    return;
  }

  if ((g_time_sync_context.state == TIME_SYNC_STATE_BACKEND_WAIT) &&
      ((int32_t)(now - g_time_sync_context.deadline_ms) >= 0))
  {
    g_time_sync_context.stats.backend_failures++;
    g_time_sync_context.stats.last_error = TIME_SYNC_ERROR_BACKEND_TIMEOUT;
    time_sync_schedule(TIME_SYNC_FAILURE_RETRY_MS);
    return;
  }

  if ((g_time_sync_context.state == TIME_SYNC_STATE_IDLE) &&
      ((int32_t)(now - g_time_sync_context.next_attempt_ms) >= 0))
  {
    time_sync_start_ntp();
  }
}

void time_sync_service_poll(void)
{
  if (g_time_sync_context.callback_pending != 0U)
  {
    return;
  }

  g_time_sync_context.callback_pending = 1U;
  if (tcpip_try_callback(time_sync_tcpip_step, NULL) != ERR_OK)
  {
    g_time_sync_context.callback_pending = 0U;
  }
}

void time_sync_service_get_stats(time_sync_service_stats_t *stats)
{
  if (stats == NULL)
  {
    return;
  }

  taskENTER_CRITICAL();
  *stats = g_time_sync_context.stats;
  taskEXIT_CRITICAL();
}

uint8_t time_sync_service_apply_backend_time(uint32_t unix_time)
{
  cluster_service_state_t cluster_state;
  uint8_t accepted = 0U;

  cluster_service_get_state(&cluster_state);
  taskENTER_CRITICAL();
  if ((g_time_sync_context.state == TIME_SYNC_STATE_BACKEND_WAIT) &&
      (cluster_state.role == CLUSTER_ROLE_MASTER) &&
      (unix_time >= TIME_SYNC_MIN_UNIX) &&
      (unix_time <= TIME_SYNC_MAX_UNIX))
  {
    accepted = 1U;
  }
  taskEXIT_CRITICAL();

  if ((accepted == 0U) ||
      (cluster_service_set_unix_time(unix_time,
                                     CLUSTER_TIME_SOURCE_BACKEND) == 0U))
  {
    taskENTER_CRITICAL();
    g_time_sync_context.stats.last_error = TIME_SYNC_ERROR_BACKEND_VALUE;
    taskEXIT_CRITICAL();
    return 0U;
  }

  taskENTER_CRITICAL();
  g_time_sync_context.stats.backend_successes++;
  g_time_sync_context.stats.last_sync_ms = HAL_GetTick();
  g_time_sync_context.stats.last_source = CLUSTER_TIME_SOURCE_BACKEND;
  g_time_sync_context.stats.last_error = TIME_SYNC_ERROR_NONE;
  g_time_sync_context.state = TIME_SYNC_STATE_IDLE;
  g_time_sync_context.deadline_ms = 0U;
  g_time_sync_context.next_attempt_ms =
    HAL_GetTick() + TIME_SYNC_BACKEND_RETRY_NTP_MS;
  g_time_sync_context.stats.active = 0U;
  g_time_sync_context.stats.waiting_backend = 0U;
  g_time_sync_context.stats.next_attempt_ms =
    g_time_sync_context.next_attempt_ms;
  taskEXIT_CRITICAL();
  return 1U;
}
