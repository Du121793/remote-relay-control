/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : LWIP.h
  * Description        : This file provides code for the configuration
  *                      of the LWIP.
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
  *************************************************************************

  */
/* USER CODE END Header */
/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __mx_lwip_H
#define __mx_lwip_H
#ifdef __cplusplus
 extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "lwip/opt.h"
#include "lwip/mem.h"
#include "lwip/memp.h"
#include "netif/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/netif.h"
#include "lwip/timeouts.h"
#include "ethernetif.h"

/* Includes for RTOS ---------------------------------------------------------*/
#if WITH_RTOS
#include "lwip/tcpip.h"
#endif /* WITH_RTOS */

/* USER CODE BEGIN 0 */
extern volatile uint32_t g_eth_tx_packets;
extern volatile uint32_t g_eth_tx_errors;
extern volatile uint32_t g_eth_rx_packets;
extern volatile uint32_t g_eth_rx_null_reads;
extern volatile uint32_t g_eth_rx_alloc_failures;
extern volatile uint32_t g_eth_rx_alloc_recoveries;
extern volatile uint32_t g_eth_rx_alloc_stalled;
extern volatile uint32_t g_eth_last_rx_ms;
extern volatile uint32_t g_eth_rx_self_packets;
extern volatile uint32_t g_eth_rx_arp_packets;
extern volatile uint32_t g_eth_rx_ipv4_packets;
extern volatile uint32_t g_eth_rx_udp_packets;
extern volatile uint32_t g_eth_rx_dhcp_like_packets;
extern volatile uint32_t g_eth_last_rx_ethertype;
extern volatile uint32_t g_eth_last_rx_ip_proto;
extern volatile uint32_t g_eth_last_rx_udp_src_port;
extern volatile uint32_t g_eth_last_rx_udp_dst_port;
extern volatile uint32_t g_eth_tx_ipv4_packets;
extern volatile uint32_t g_eth_tx_udp_packets;
extern volatile uint32_t g_eth_tx_dhcp_like_packets;
extern volatile uint32_t g_eth_last_tx_ethertype;
extern volatile uint32_t g_eth_last_tx_ip_proto;
extern volatile uint32_t g_eth_last_tx_udp_src_port;
extern volatile uint32_t g_eth_last_tx_udp_dst_port;
extern volatile uint32_t g_eth_last_tx_ip_src;
extern volatile uint32_t g_eth_last_tx_ip_dst;
extern volatile uint32_t g_eth_last_tx_dst_mac_0_1;
extern volatile uint32_t g_eth_last_tx_dst_mac_2_5;
extern volatile uint32_t g_eth_tx_free_packets;
extern volatile uint32_t g_eth_last_tx_hal_status;
extern volatile uint32_t g_eth_last_tx_hal_error;
extern volatile uint32_t g_eth_last_tx_dma_error;
extern volatile uint32_t g_eth_tx_reclaim_calls;
extern volatile uint32_t g_eth_tx_reclaim_schedule_errors;

/* USER CODE END 0 */

/* Global Variables ----------------------------------------------------------*/
extern ETH_HandleTypeDef heth;

/* LWIP init function */
void MX_LWIP_Init(void);

#if !WITH_RTOS
/* USER CODE BEGIN 1 */
/* Function defined in lwip.c to:
 *   - Read a received packet from the Ethernet buffers
 *   - Send it to the lwIP stack for handling
 *   - Handle timeouts if NO_SYS_NO_TIMERS not set
 */
void MX_LWIP_Process(void);

/* USER CODE END 1 */
#endif /* WITH_RTOS */

#ifdef __cplusplus
}
#endif
#endif /*__ mx_lwip_H */

/**
  * @}
  */

/**
  * @}
  */
