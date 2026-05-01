/* USER CODE BEGIN Header */
/**
 ******************************************************************************
  * File Name          : LWIP.c
  * Description        : This file provides initialization code for LWIP
  *                      middleWare.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "lwip.h"
#include "lwip/init.h"
#include "lwip/igmp.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/pbuf.h"
#include "lwip/udp.h"
#include "lwip/ip_addr.h"
#include <stdio.h>
#include <string.h>
#if defined ( __CC_ARM )  /* MDK ARM Compiler */
#include "lwip/sio.h"
#endif /* MDK ARM Compiler */
#include "ethernetif.h"

/* USER CODE BEGIN 0 */
static void discovery_udp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port);
static void discovery_udp_send_announce(const ip_addr_t *addr, u16_t port);
static void discovery_udp_send_multicast_announce(void);
static void discovery_udp_send_broadcast_announce(void);
static void discovery_udp_poll(void);
static void discovery_udp_ensure_multicast_membership(void);

#define DISCOVERY_UDP_PORT              40000U
#define DISCOVERY_ANNOUNCE_INTERVAL_MS  5000U
#define DISCOVERY_SERVICE_TYPE          "autoControl"
#define DISCOVERY_CLUSTER_ID            "default"
#define DISCOVERY_DEVICE_ID             "stm32f767zi"
#define DISCOVERY_PROTOCOL_VERSION      "1"
#define DISCOVERY_MULTICAST_IP0         239
#define DISCOVERY_MULTICAST_IP1         255
#define DISCOVERY_MULTICAST_IP2         42
#define DISCOVERY_MULTICAST_IP3         99

extern struct netif gnetif;

volatile uint32_t g_udp_discovery_rx_packets = 0U;
volatile uint32_t g_udp_discovery_announce_packets = 0U;
volatile uint32_t g_udp_discovery_discover_packets = 0U;
volatile uint32_t g_udp_discovery_last_src_ip = 0U;
volatile uint32_t g_udp_discovery_last_src_port = 0U;

static struct udp_pcb *g_discovery_pcb = NULL;
static uint32_t g_discovery_next_announce_ms = 0U;
static uint8_t g_discovery_last_link_up = 0U;
static uint8_t g_discovery_multicast_joined = 0U;
static ip4_addr_t g_discovery_group_addr;

static void discovery_udp_send_announce(const ip_addr_t *addr, u16_t port)
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
  payload_len = snprintf(payload, sizeof(payload),
                         "type=announce ver=%s service=%s cluster=%s device=%s ip=%s port=%u mac=%02X:%02X:%02X:%02X:%02X:%02X",
                         DISCOVERY_PROTOCOL_VERSION,
                         DISCOVERY_SERVICE_TYPE,
                         DISCOVERY_CLUSTER_ID,
                         DISCOVERY_DEVICE_ID,
                         ip_text,
                         (unsigned int)DISCOVERY_UDP_PORT,
                         gnetif.hwaddr[0], gnetif.hwaddr[1], gnetif.hwaddr[2],
                         gnetif.hwaddr[3], gnetif.hwaddr[4], gnetif.hwaddr[5]);
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
    g_udp_discovery_announce_packets++;
  }

  pbuf_free(pb);
}

static void discovery_udp_send_multicast_announce(void)
{
  ip_addr_t group_addr;

  ip_addr_copy_from_ip4(group_addr, g_discovery_group_addr);
  discovery_udp_send_announce(&group_addr, DISCOVERY_UDP_PORT);
}

static void discovery_udp_send_broadcast_announce(void)
{
  discovery_udp_send_announce(NULL, DISCOVERY_UDP_PORT);
}

static void discovery_udp_ensure_multicast_membership(void)
{
  if (!g_discovery_multicast_joined && dhcp_supplied_address(&gnetif))
  {
    if (igmp_joingroup_netif(&gnetif, &g_discovery_group_addr) == ERR_OK)
    {
      g_discovery_multicast_joined = 1U;
    }
  }
}

static void discovery_udp_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
  char payload[96];
  u16_t copy_len;

  LWIP_UNUSED_ARG(arg);
  LWIP_UNUSED_ARG(pcb);

  if (p == NULL)
  {
    return;
  }

  g_udp_discovery_rx_packets++;
  g_udp_discovery_last_src_port = port;
  g_udp_discovery_last_src_ip = ip4_addr_get_u32(ip_2_ip4(addr));
  copy_len = (p->tot_len < (sizeof(payload) - 1U)) ? p->tot_len : (u16_t)(sizeof(payload) - 1U);
  pbuf_copy_partial(p, payload, copy_len, 0U);
  payload[copy_len] = '\0';

  if (strncmp(payload, "DISCOVER", 8) == 0)
  {
    g_udp_discovery_discover_packets++;
    discovery_udp_send_announce(addr, port);
  }

  pbuf_free(p);
}

static void discovery_udp_poll(void)
{
  uint8_t link_up = netif_is_link_up(&gnetif) ? 1U : 0U;
  uint32_t now = HAL_GetTick();

  if (!link_up)
  {
    g_discovery_last_link_up = 0U;
    g_discovery_multicast_joined = 0U;
    return;
  }

  if (dhcp_supplied_address(&gnetif))
  {
    discovery_udp_ensure_multicast_membership();
    if (g_discovery_last_link_up == 0U)
    {
      discovery_udp_send_multicast_announce();
      discovery_udp_send_broadcast_announce();
      g_discovery_next_announce_ms = now + DISCOVERY_ANNOUNCE_INTERVAL_MS;
      g_discovery_last_link_up = 1U;
      return;
    }

    if ((int32_t)(now - g_discovery_next_announce_ms) >= 0)
    {
      discovery_udp_send_multicast_announce();
      discovery_udp_send_broadcast_announce();
      g_discovery_next_announce_ms = now + DISCOVERY_ANNOUNCE_INTERVAL_MS;
    }
  }
}

/* USER CODE END 0 */
/* Private function prototypes -----------------------------------------------*/
static void ethernet_link_status_updated(struct netif *netif);
static void Ethernet_Link_Periodic_Handle(struct netif *netif);
/* ETH Variables initialization ----------------------------------------------*/
void Error_Handler(void);

/* DHCP Variables initialization ---------------------------------------------*/
uint32_t DHCPfineTimer = 0;
uint32_t DHCPcoarseTimer = 0;
/* USER CODE BEGIN 1 */

/* USER CODE END 1 */
uint32_t EthernetLinkTimer;

/* Variables Initialization */
struct netif gnetif;
ip4_addr_t ipaddr;
ip4_addr_t netmask;
ip4_addr_t gw;

/* USER CODE BEGIN 2 */
/* USER CODE END 2 */

/**
  * LwIP initialization function
  */
void MX_LWIP_Init(void)
{
/* USER CODE BEGIN Init */
  IP4_ADDR(&g_discovery_group_addr,
           DISCOVERY_MULTICAST_IP0,
           DISCOVERY_MULTICAST_IP1,
           DISCOVERY_MULTICAST_IP2,
           DISCOVERY_MULTICAST_IP3);
/* USER CODE END Init */

  /* Initialize the LwIP stack without RTOS */
  lwip_init();

  /* IP addresses initialization with DHCP (IPv4) */
  ipaddr.addr = 0;
  netmask.addr = 0;
  gw.addr = 0;

  /* add the network interface (IPv4/IPv6) without RTOS */
  netif_add(&gnetif, &ipaddr, &netmask, &gw, NULL, &ethernetif_init, &ethernet_input);

  /* Registers the default network interface */
  netif_set_default(&gnetif);

  /* We must always bring the network interface up connection or not... */
  netif_set_up(&gnetif);

  /* Set the link callback function, this function is called on change of link status*/
  netif_set_link_callback(&gnetif, ethernet_link_status_updated);

  /* Start DHCP negotiation for a network interface (IPv4) */
  dhcp_start(&gnetif);

/* USER CODE BEGIN 3 */
  g_discovery_pcb = udp_new_ip_type(IPADDR_TYPE_V4);
  if (g_discovery_pcb != NULL)
  {
    ip_set_option(g_discovery_pcb, SOF_REUSEADDR);
    (void)udp_bind(g_discovery_pcb, IP4_ADDR_ANY, DISCOVERY_UDP_PORT);
    ip_set_option(g_discovery_pcb, SOF_BROADCAST);
    udp_recv(g_discovery_pcb, discovery_udp_recv, NULL);
  }

/* USER CODE END 3 */
}

#ifdef USE_OBSOLETE_USER_CODE_SECTION_4
/* Kept to help code migration. (See new 4_1, 4_2... sections) */
/* Avoid to use this user section which will become obsolete. */
/* USER CODE BEGIN 4 */
/* USER CODE END 4 */
#endif

/**
  * @brief  Ethernet Link periodic check
  * @param  netif
  * @retval None
  */
static void Ethernet_Link_Periodic_Handle(struct netif *netif)
{
/* USER CODE BEGIN 4_4_1 */
/* USER CODE END 4_4_1 */

  /* Ethernet Link every 100ms */
  if (HAL_GetTick() - EthernetLinkTimer >= 100)
  {
    EthernetLinkTimer = HAL_GetTick();
    ethernet_link_check_state(netif);
  }
/* USER CODE BEGIN 4_4 */
/* USER CODE END 4_4 */
}

/**
 * ----------------------------------------------------------------------
 * Function given to help user to continue LwIP Initialization
 * Up to user to complete or change this function ...
 * Up to user to call this function in main.c in while (1) of main(void)
 *-----------------------------------------------------------------------
 * Read a received packet from the Ethernet buffers
 * Send it to the lwIP stack for handling
 * Handle timeouts if LWIP_TIMERS is set and without RTOS
 * Handle the llink status if LWIP_NETIF_LINK_CALLBACK is set and without RTOS
 */
void MX_LWIP_Process(void)
{
/* USER CODE BEGIN 4_1 */
/* USER CODE END 4_1 */
  ethernetif_input(&gnetif);

/* USER CODE BEGIN 4_2 */
/* USER CODE END 4_2 */
  /* Handle timeouts */
  sys_check_timeouts();

  Ethernet_Link_Periodic_Handle(&gnetif);

/* USER CODE BEGIN 4_3 */
  discovery_udp_poll();
/* USER CODE END 4_3 */
}

/**
  * @brief  Notify the User about the network interface config status
  * @param  netif: the network interface
  * @retval None
  */
static void ethernet_link_status_updated(struct netif *netif)
{
  if (netif_is_up(netif))
  {
/* USER CODE BEGIN 5 */
/* USER CODE END 5 */
  }
  else /* netif is down */
  {
/* USER CODE BEGIN 6 */
/* USER CODE END 6 */
  }
}

#if defined ( __CC_ARM )  /* MDK ARM Compiler */
/**
 * Opens a serial device for communication.
 *
 * @param devnum device number
 * @return handle to serial device if successful, NULL otherwise
 */
sio_fd_t sio_open(u8_t devnum)
{
  sio_fd_t sd;

/* USER CODE BEGIN 7 */
  sd = 0; // dummy code
/* USER CODE END 7 */

  return sd;
}

/**
 * Sends a single character to the serial device.
 *
 * @param c character to send
 * @param fd serial device handle
 *
 * @note This function will block until the character can be sent.
 */
void sio_send(u8_t c, sio_fd_t fd)
{
/* USER CODE BEGIN 8 */
/* USER CODE END 8 */
}

/**
 * Reads from the serial device.
 *
 * @param fd serial device handle
 * @param data pointer to data buffer for receiving
 * @param len maximum length (in bytes) of data to receive
 * @return number of bytes actually received - may be 0 if aborted by sio_read_abort
 *
 * @note This function will block until data can be received. The blocking
 * can be cancelled by calling sio_read_abort().
 */
u32_t sio_read(sio_fd_t fd, u8_t *data, u32_t len)
{
  u32_t recved_bytes;

/* USER CODE BEGIN 9 */
  recved_bytes = 0; // dummy code
/* USER CODE END 9 */
  return recved_bytes;
}

/**
 * Tries to read from the serial device. Same as sio_read but returns
 * immediately if no data is available and never blocks.
 *
 * @param fd serial device handle
 * @param data pointer to data buffer for receiving
 * @param len maximum length (in bytes) of data to receive
 * @return number of bytes actually received
 */
u32_t sio_tryread(sio_fd_t fd, u8_t *data, u32_t len)
{
  u32_t recved_bytes;

/* USER CODE BEGIN 10 */
  recved_bytes = 0; // dummy code
/* USER CODE END 10 */
  return recved_bytes;
}
#endif /* MDK ARM Compiler */

