#include "discovery_service.h"

#include "cmsis_os.h"

#include "board_relay.h"
#include "adc_service.h"
#include "cluster_service.h"
#include "control_service.h"
#include "../Config/device_config.h"
#include "discovery_proto.h"
#include "mbedtls/md.h"
#include "http_service.h"
#include "lwip.h"

#include "lwip/igmp.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcpip.h"
#include "lwip/udp.h"

#include <stdio.h>
#include <string.h>

#define DISCOVERY_UDP_PORT              40000U
#define DISCOVERY_ANNOUNCE_INTERVAL_MS  2000U
#define DISCOVERY_SERVICE_TYPE          "autoControl"
#define DISCOVERY_MULTICAST_IP0         239
#define DISCOVERY_MULTICAST_IP1         255
#define DISCOVERY_MULTICAST_IP2         42
#define DISCOVERY_MULTICAST_IP3         99
#define DISCOVERY_ANNOUNCE_BUFFER_SIZE  DISCOVERY_PROTO_ANNOUNCE_MAX_SIZE

extern struct netif gnetif;

static struct udp_pcb *g_discovery_pcb = NULL;
static uint32_t g_discovery_next_announce_ms = 0U;
static uint8_t g_discovery_last_link_up = 0U;
static uint8_t g_discovery_multicast_joined = 0U;
static ip4_addr_t g_discovery_group_addr;
static discovery_service_stats_t g_discovery_stats;
static char g_discovery_rx_payload[DISCOVERY_ANNOUNCE_BUFFER_SIZE];
static osThreadId g_discovery_task_handle = NULL;
static volatile uint8_t g_discovery_force_announce = 0U;

static void discovery_service_tcpip_step(void *arg);
static void discovery_service_task(void const *argument);

static void discovery_service_udp_send_announce(const ip_addr_t *addr, u16_t port)
{
  char payload[DISCOVERY_ANNOUNCE_BUFFER_SIZE];
  char ip_text[16];
  int payload_len;
  err_t send_err;
  struct pbuf *pb;
  ip_addr_t target_addr;
  cluster_service_state_t cluster_state;
  http_service_stats_t http_stats;
  control_service_stats_t control_stats;
  uint32_t load_score;
  uint32_t auth_tag;
  uint32_t enabled_mask = 0U;
  adc_service_state_t adc_state;
  const device_config_t *config = device_config_get();
  uint32_t relay_index;

  if ((g_discovery_pcb == NULL) || !netif_is_link_up(&gnetif) || !dhcp_supplied_address(&gnetif))
  {
    return;
  }

  cluster_service_get_state(&cluster_state);
  adc_service_get_state(&adc_state);
  http_service_get_stats(&http_stats);
  control_service_get_stats(&control_stats);
  load_score = cluster_service_compute_load_score(http_stats.active_connections,
                                                  control_stats.connections,
                                                  control_stats.commands);
  cluster_service_set_local_load_score(load_score);
  {
    /* HMAC-SHA256 anti-replay: bind auth_tag to the current unix_time,
       node_id and cluster_name so an attacker cannot capture and replay
       an old announce.  The receiver checks timestamp ±30 seconds. */
    uint8_t hmac_key[DEVICE_CONFIG_PASSWORD_HASH_LEN];
    uint8_t hmac_out[32];
    char hmac_input[128];
    int hmac_len;
    memcpy(hmac_key, config->cluster_password_hash, sizeof(hmac_key));
    hmac_len = snprintf(hmac_input, sizeof(hmac_input),
                        "%lu|%lu|%s",
                        (unsigned long)cluster_state.unix_time,
                        (unsigned long)cluster_state.self_node_id,
                        cluster_state.cluster_name);
    if ((hmac_len > 0) &&
        ((size_t)hmac_len < sizeof(hmac_input)) &&
        (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                         hmac_key, sizeof(hmac_key),
                         (const unsigned char *)hmac_input, (size_t)hmac_len,
                         hmac_out) == 0))
    {
      memcpy(&auth_tag, hmac_out, sizeof(auth_tag));
    }
    else
    {
      auth_tag = 0U;
    }
  }
  for (relay_index = 0U; relay_index < DEVICE_CONFIG_RELAY_COUNT; ++relay_index)
  {
    if (config->relay[relay_index].enabled != 0U)
    {
      enabled_mask |= (1UL << relay_index);
    }
  }
  ip4addr_ntoa_r(netif_ip4_addr(&gnetif), ip_text, sizeof(ip_text));
  payload_len = discovery_proto_build_announce(payload,
                                               sizeof(payload),
                                               DISCOVERY_SERVICE_TYPE,
                                               cluster_state.cluster_name,
                                               config->device_name,
                                               config->cloud_cluster_id,
                                               config->cloud_device_id,
                                               config->cloud_key_id,
                                               cluster_state.self_node_id,
                                               cluster_state.role,
                                               cluster_state.term,
                                               load_score,
                                               auth_tag,
                                               board_relay_snapshot(),
                                               enabled_mask,
                                               config->auto_join_cluster,
                                               cluster_state.unix_time,
                                               cluster_state.time_valid,
                                               cluster_state.rtc_lse_ready,
                                               cluster_state.time_source,
                                               cluster_state.time_quality,
                                               cluster_state.time_seq,
                                               cluster_state.gateway_epoch,
                                               cluster_state.gateway_node_id,
                                               adc_state.valid,
                                               adc_state.ct_valid,
                                               adc_state.signal_present,
                                               adc_state.raw_latest,
                                               adc_state.millivolts,
                                               adc_state.raw_min,
                                               adc_state.raw_max,
                                               adc_state.raw_average,
                                               adc_state.raw_rms,
                                               adc_state.ac_rms_millivolts,
                                               adc_state.current_milliamps,
                                               adc_state.sample_rate_hz,
                                               adc_state.window_samples,
                                               adc_state.sample_count,
                                               adc_state.dma_overrun_count,
                                               adc_state.saturation_count,
                                               ip_text,
                                               DISCOVERY_UDP_PORT,
                                               gnetif.hwaddr);
  g_discovery_stats.last_payload_len = (payload_len > 0) ? (uint32_t)payload_len : 0U;
  if ((payload_len <= 0) || ((size_t)payload_len >= sizeof(payload)))
  {
    g_discovery_stats.tx_failures++;
    g_discovery_stats.last_tx_err = ERR_BUF;
    return;
  }

  pb = pbuf_alloc(PBUF_TRANSPORT, (u16_t)payload_len, PBUF_RAM);
  if (pb == NULL)
  {
    return;
  }

  memcpy(pb->payload, payload, (size_t)payload_len);
  if (addr != NULL)
  {
    ip_addr_copy(target_addr, *addr);
  }
  else
  {
    IP_ADDR4(&target_addr, 255, 255, 255, 255);
    port = DISCOVERY_UDP_PORT;
  }

  g_discovery_stats.tx_attempts++;
  send_err = udp_sendto(g_discovery_pcb, pb, &target_addr, port);
  if (send_err == ERR_OK)
  {
    g_discovery_stats.announce_packets++;
    g_discovery_stats.last_tx_err = ERR_OK;
  }
  else
  {
    g_discovery_stats.tx_failures++;
    g_discovery_stats.last_tx_err = (int32_t)send_err;
  }

  pbuf_free(pb);
}

static void discovery_service_send_multicast_announce(void)
{
  ip_addr_t group_addr;

  ip_addr_copy_from_ip4(group_addr, g_discovery_group_addr);
  discovery_service_udp_send_announce(&group_addr, DISCOVERY_UDP_PORT);
}

static void discovery_service_send_broadcast_announce(void)
{
  discovery_service_udp_send_announce(NULL, DISCOVERY_UDP_PORT);
}

static void discovery_service_ensure_multicast_membership(void)
{
  if (!g_discovery_multicast_joined && dhcp_supplied_address(&gnetif))
  {
    if (igmp_joingroup_netif(&gnetif, &g_discovery_group_addr) == ERR_OK)
    {
      g_discovery_multicast_joined = 1U;
    }
  }
}

static void discovery_service_udp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
  u16_t copy_len;
  discovery_proto_announce_t announce;
  cluster_node_t node;

  LWIP_UNUSED_ARG(arg);
  LWIP_UNUSED_ARG(pcb);

  if (p == NULL)
  {
    return;
  }

  g_discovery_stats.rx_packets++;
  g_discovery_stats.last_src_port = port;
  g_discovery_stats.last_src_ip = ip4_addr_get_u32(ip_2_ip4(addr));
  copy_len = (p->tot_len < (sizeof(g_discovery_rx_payload) - 1U)) ?
             p->tot_len :
             (u16_t)(sizeof(g_discovery_rx_payload) - 1U);
  pbuf_copy_partial(p, g_discovery_rx_payload, copy_len, 0U);
  g_discovery_rx_payload[copy_len] = '\0';

  if (discovery_proto_is_discover(g_discovery_rx_payload, copy_len) != 0U)
  {
    g_discovery_stats.discover_packets++;
    discovery_service_udp_send_announce(addr, port);
  }
  else if (discovery_proto_parse_announce(g_discovery_rx_payload,
                                          copy_len,
                                          &announce) != 0U)
  {
    if (strcmp(announce.service, DISCOVERY_SERVICE_TYPE) == 0)
    {
      memset(&node, 0, sizeof(node));
      node.valid = 1U;
      node.online = 1U;
      node.auto_join = announce.auto_join;
      node.role = announce.role;
      node.device_state = CLUSTER_DEVICE_STATE_RUNNING;
      node.node_id = announce.node_id;
      node.ip_addr = ip4_addr_get_u32(ip_2_ip4(addr));
      node.last_seen_ms = HAL_GetTick();
      node.load_score = announce.load_score;
      node.term = announce.term;
      node.relay_mask = announce.relay_mask;
      node.enabled_mask = announce.enabled_mask;
      node.auth_tag = announce.auth_tag;
      node.unix_time = announce.unix_time;
      node.time_valid = announce.time_valid;
      node.time_source = announce.time_source;
      node.time_quality = announce.time_quality;
      node.time_seq = announce.time_seq;
      node.gateway_epoch = announce.gateway_epoch;
      node.gateway_node_id = announce.gateway_node_id;
      node.adc_valid = announce.adc_valid;
      node.adc_ct_valid = announce.adc_ct_valid;
      node.adc_signal_present = announce.adc_signal_present;
      node.adc_raw = announce.adc_raw;
      node.adc_millivolts = announce.adc_millivolts;
      node.adc_min = announce.adc_min;
      node.adc_max = announce.adc_max;
      node.adc_average = announce.adc_average;
      node.adc_raw_rms = announce.adc_raw_rms;
      node.adc_rms_millivolts = announce.adc_rms_millivolts;
      node.adc_current_milliamps = announce.adc_current_milliamps;
      node.adc_sample_rate_hz = announce.adc_sample_rate_hz;
      node.adc_window_samples = announce.adc_window_samples;
      node.adc_sample_count = announce.adc_sample_count;
      node.adc_dma_overruns = announce.adc_dma_overruns;
      node.adc_saturation_count = announce.adc_saturation_count;
      memcpy(node.node_name, announce.device, sizeof(node.node_name));
      memcpy(node.cluster_name, announce.cluster, sizeof(node.cluster_name));
      memcpy(node.cloud_cluster_id, announce.cloud_cluster_id, sizeof(node.cloud_cluster_id));
      memcpy(node.cloud_device_id, announce.cloud_device_id, sizeof(node.cloud_device_id));
      memcpy(node.cloud_key_id, announce.cloud_key_id, sizeof(node.cloud_key_id));
      memcpy(node.mac, announce.mac, sizeof(node.mac));
      cluster_service_note_remote_node(&node);
    }
  }

  pbuf_free(p);
}

void discovery_service_init(void)
{
  IP4_ADDR(&g_discovery_group_addr,
           DISCOVERY_MULTICAST_IP0,
           DISCOVERY_MULTICAST_IP1,
           DISCOVERY_MULTICAST_IP2,
           DISCOVERY_MULTICAST_IP3);

  if (g_discovery_task_handle == NULL)
  {
    osThreadDef(DiscoveryService, discovery_service_task, osPriorityBelowNormal, 0, 1024);
    g_discovery_task_handle = osThreadCreate(osThread(DiscoveryService), NULL);
  }
}

void discovery_service_poll(void)
{
  if (netif_is_link_up(&gnetif) == 0)
  {
    return;
  }
  discovery_service_init();
}

void discovery_service_force_announce(void)
{
  g_discovery_force_announce = 1U;
}

static void discovery_service_tcpip_step(void *arg)
{
  LWIP_UNUSED_ARG(arg);
  uint8_t link_up = netif_is_link_up(&gnetif) ? 1U : 0U;
  uint32_t now = HAL_GetTick();

  if (g_discovery_pcb == NULL)
  {
    g_discovery_pcb = udp_new_ip_type(IPADDR_TYPE_V4);
    if (g_discovery_pcb != NULL)
    {
      ip_set_option(g_discovery_pcb, SOF_REUSEADDR);
      (void)udp_bind(g_discovery_pcb, IP4_ADDR_ANY, DISCOVERY_UDP_PORT);
      ip_set_option(g_discovery_pcb, SOF_BROADCAST);
      udp_recv(g_discovery_pcb, discovery_service_udp_recv, NULL);
    }
  }

  if (!link_up)
  {
    if (g_discovery_multicast_joined)
    {
      (void)igmp_leavegroup_netif(&gnetif, &g_discovery_group_addr);
    }
    g_discovery_last_link_up = 0U;
    g_discovery_multicast_joined = 0U;
    return;
  }

  if (dhcp_supplied_address(&gnetif))
  {
    discovery_service_ensure_multicast_membership();
    if (g_discovery_last_link_up == 0U)
    {
      discovery_service_send_multicast_announce();
      discovery_service_send_broadcast_announce();
      g_discovery_next_announce_ms = now + DISCOVERY_ANNOUNCE_INTERVAL_MS;
      g_discovery_last_link_up = 1U;
      return;
    }

    if ((int32_t)(now - g_discovery_next_announce_ms) >= 0)
    {
      discovery_service_send_multicast_announce();
      discovery_service_send_broadcast_announce();
      g_discovery_next_announce_ms = now + DISCOVERY_ANNOUNCE_INTERVAL_MS;
    }

    if (g_discovery_force_announce != 0U)
    {
      g_discovery_force_announce = 0U;
      discovery_service_send_multicast_announce();
      discovery_service_send_broadcast_announce();
      g_discovery_next_announce_ms = now + DISCOVERY_ANNOUNCE_INTERVAL_MS;
    }
  }
}

static void discovery_service_task(void const *argument)
{
  (void)argument;

  for (;;)
  {
    (void)tcpip_callback(discovery_service_tcpip_step, NULL);
    osDelay(500U);
  }
}

void discovery_service_get_stats(discovery_service_stats_t *stats)
{
  if (stats != NULL)
  {
    *stats = g_discovery_stats;
  }
}
