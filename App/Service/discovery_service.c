#include "discovery_service.h"

#include "cmsis_os.h"

#include "discovery_proto.h"
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
#define DISCOVERY_ANNOUNCE_INTERVAL_MS  5000U
#define DISCOVERY_SERVICE_TYPE          "autoControl"
#define DISCOVERY_CLUSTER_ID            "default"
#define DISCOVERY_DEVICE_ID             "stm32f767zi"
#define DISCOVERY_MULTICAST_IP0         239
#define DISCOVERY_MULTICAST_IP1         255
#define DISCOVERY_MULTICAST_IP2         42
#define DISCOVERY_MULTICAST_IP3         99

extern struct netif gnetif;

static struct udp_pcb *g_discovery_pcb = NULL;
static uint32_t g_discovery_next_announce_ms = 0U;
static uint8_t g_discovery_last_link_up = 0U;
static uint8_t g_discovery_multicast_joined = 0U;
static ip4_addr_t g_discovery_group_addr;
static discovery_service_stats_t g_discovery_stats;
static osThreadId g_discovery_task_handle = NULL;

static void discovery_service_tcpip_step(void *arg);
static void discovery_service_task(void const *argument);

static void discovery_service_udp_send_announce(const ip_addr_t *addr, u16_t port)
{
  char payload[192];
  char ip_text[16];
  int payload_len;
  struct pbuf *pb;
  ip_addr_t target_addr;

  if ((g_discovery_pcb == NULL) || !netif_is_link_up(&gnetif) || !dhcp_supplied_address(&gnetif))
  {
    return;
  }

  ip4addr_ntoa_r(netif_ip4_addr(&gnetif), ip_text, sizeof(ip_text));
  payload_len = discovery_proto_build_announce(payload,
                                               sizeof(payload),
                                               DISCOVERY_SERVICE_TYPE,
                                               DISCOVERY_CLUSTER_ID,
                                               DISCOVERY_DEVICE_ID,
                                               ip_text,
                                               DISCOVERY_UDP_PORT,
                                               gnetif.hwaddr);
  if ((payload_len <= 0) || ((size_t)payload_len >= sizeof(payload)))
  {
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

  if (udp_sendto(g_discovery_pcb, pb, &target_addr, port) == ERR_OK)
  {
    g_discovery_stats.announce_packets++;
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
  char payload[96];
  u16_t copy_len;

  LWIP_UNUSED_ARG(arg);
  LWIP_UNUSED_ARG(pcb);

  if (p == NULL)
  {
    return;
  }

  g_discovery_stats.rx_packets++;
  g_discovery_stats.last_src_port = port;
  g_discovery_stats.last_src_ip = ip4_addr_get_u32(ip_2_ip4(addr));
  copy_len = (p->tot_len < (sizeof(payload) - 1U)) ? p->tot_len : (u16_t)(sizeof(payload) - 1U);
  pbuf_copy_partial(p, payload, copy_len, 0U);
  payload[copy_len] = '\0';

  if (discovery_proto_is_discover(payload, copy_len) != 0U)
  {
    g_discovery_stats.discover_packets++;
    discovery_service_udp_send_announce(addr, port);
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
    osThreadDef(DiscoveryService, discovery_service_task, osPriorityBelowNormal, 0, 256);
    g_discovery_task_handle = osThreadCreate(osThread(DiscoveryService), NULL);
  }
}

void discovery_service_poll(void)
{
  discovery_service_init();
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
