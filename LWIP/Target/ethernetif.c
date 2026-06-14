/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : ethernetif.c
  * Description        : This file provides code for the configuration
  *                      of the ethernetif.c MiddleWare.
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
#include "main.h"
#include "lwip/opt.h"
#include "lwip/timeouts.h"
#include "lwip/dhcp.h"
#include "netif/ethernet.h"
#include "netif/etharp.h"
#include "lwip/ethip6.h"
#include "ethernetif.h"
#include "lan8742.h"
#include <string.h>
#include "cmsis_os.h"
#include "lwip/tcpip.h"

/* Within 'USER CODE' section, code will be kept by default at each generation */
/* USER CODE BEGIN 0 */
static void ethernetif_build_mac_address(uint8_t *mac_addr);
static void ethernetif_relax_rx_filter(void);
static void ethernetif_relax_rx_processing(void);
static void ethernetif_force_phy_ready(void);
__attribute__((noinline, optnone)) static void ethernetif_init_custom_pbuf_safe(struct pbuf_custom *custom_pbuf, void *payload_mem, u16_t payload_len);
__attribute__((noinline, optnone)) static void ethernetif_copy_bytes_safe(void *dst, const void *src, size_t len);
static void ethernetif_uart_write(const char *text);
static void ethernetif_netif_up_callback(void *ctx);
static void ethernetif_netif_down_callback(void *ctx);
static void ethernetif_tx_reclaim_callback(void *ctx);

extern UART_HandleTypeDef huart3;

__attribute__((noinline, optnone)) static void ethernetif_copy_bytes_safe(void *dst, const void *src, size_t len)
{
  volatile uint8_t *dst_bytes = (volatile uint8_t *)dst;
  const volatile uint8_t *src_bytes = (const volatile uint8_t *)src;
  size_t i;

  for (i = 0U; i < len; ++i)
  {
    dst_bytes[i] = src_bytes[i];
  }
}

static void ethernetif_uart_write(const char *text)
{
  size_t len = strlen(text);

  if ((len > 0U) && (huart3.Instance != NULL))
  {
    (void)HAL_UART_Transmit(&huart3, (uint8_t *)text, (uint16_t)len, 100U);
  }
}

__attribute__((noinline, optnone)) static void ethernetif_init_custom_pbuf_safe(struct pbuf_custom *custom_pbuf, void *payload_mem, u16_t payload_len)
{
  struct pbuf *pbuf = &custom_pbuf->pbuf;

  pbuf->next = NULL;
  pbuf->payload = payload_mem;
  pbuf->tot_len = payload_len;
  pbuf->len = payload_len;
  pbuf->type_internal = (u8_t)PBUF_REF;
  pbuf->flags = PBUF_FLAG_IS_CUSTOM;
  pbuf->ref = 1;
  pbuf->if_idx = NETIF_NO_INDEX;
}

static void ethernetif_netif_up_callback(void *ctx)
{
  struct netif *netif = (struct netif *)ctx;

  if (netif_is_up(netif) == 0)
  {
    netif_set_up(netif);
  }

  if (netif_is_link_up(netif) == 0)
  {
    netif_set_link_up(netif);
  }

  if (dhcp_supplied_address(netif) == 0)
  {
    dhcp_start(netif);
  }
}

static void ethernetif_netif_down_callback(void *ctx)
{
  struct netif *netif = (struct netif *)ctx;

  if (netif_is_link_up(netif) != 0)
  {
    netif_set_link_down(netif);
  }

  if (netif_is_up(netif) != 0)
  {
    netif_set_down(netif);
  }
}

/* USER CODE END 0 */

/* Private define ------------------------------------------------------------*/
/* The time to block waiting for input. */
#define TIME_WAITING_FOR_INPUT ( portMAX_DELAY )
#define TIME_WAITING_FOR_INPUT_POLL_MS ( 10U )
/* Time to block waiting for transmissions to finish */
#define ETHIF_TX_TIMEOUT (2000U)
/* USER CODE BEGIN OS_THREAD_STACK_SIZE_WITH_RTOS */
/* Stack size of the interface thread */
#define INTERFACE_THREAD_STACK_SIZE ( 350 )
/* USER CODE END OS_THREAD_STACK_SIZE_WITH_RTOS */
/* Network interface name */
#define IFNAME0 's'
#define IFNAME1 't'

/* ETH Setting  */
#define ETH_DMA_TRANSMIT_TIMEOUT               ( 20U )
#define ETH_TX_BUFFER_MAX             ((ETH_TX_DESC_CNT) * 2U)
#define ETH_TX_DMA_BUFFER_SIZE                1536U

/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/* Private variables ---------------------------------------------------------*/
/*
@Note: This interface is implemented to operate in zero-copy mode only:
        - Rx Buffers will be allocated from LwIP stack Rx memory pool,
          then passed to ETH HAL driver.
        - Tx Buffers will be allocated from LwIP stack memory heap,
          then passed to ETH HAL driver.

@Notes:
  1.a. ETH DMA Rx descriptors must be contiguous, the default count is 4,
       to customize it please redefine ETH_RX_DESC_CNT in ETH GUI (Rx Descriptor Length)
       so that updated value will be generated in stm32xxxx_hal_conf.h
  1.b. ETH DMA Tx descriptors must be contiguous, the default count is 4,
       to customize it please redefine ETH_TX_DESC_CNT in ETH GUI (Tx Descriptor Length)
       so that updated value will be generated in stm32xxxx_hal_conf.h

  2.a. Rx Buffers number must be between ETH_RX_DESC_CNT and 2*ETH_RX_DESC_CNT
  2.b. Rx Buffers must have the same size: ETH_RX_BUF_SIZE, this value must
       passed to ETH DMA in the init field (heth.Init.RxBuffLen)
  2.c  The RX Ruffers addresses and sizes must be properly defined to be aligned
       to L1-CACHE line size (32 bytes).
*/

/* Data Type Definitions */
typedef enum
{
  RX_ALLOC_OK       = 0x00,
  RX_ALLOC_ERROR    = 0x01
} RxAllocStatusTypeDef;

typedef struct
{
  struct pbuf_custom pbuf_custom;
  uint8_t buff[(ETH_RX_BUF_SIZE + 31) & ~31] __ALIGNED(32);
} RxBuff_t;

/* Memory Pool Declaration */
#define ETH_RX_BUFFER_CNT             6U
#undef LWIP_DECLARE_MEMORY_ALIGNED
#if defined ( __ARMCC_VERSION )  /* ARMCLANG */
#define LWIP_DECLARE_MEMORY_ALIGNED(variable_name, size) \
  u8_t variable_name[size] __attribute__((section(".ARM.__at_0x2007DA00"), aligned(32)))
#elif defined ( __CC_ARM )  /* MDK ARM Compiler */
#define LWIP_DECLARE_MEMORY_ALIGNED(variable_name, size) \
  __attribute__((at(0x2007DA00))) u8_t variable_name[size] __ALIGNED(32)
#elif defined ( __GNUC__ ) /* GNU Compiler */
#define LWIP_DECLARE_MEMORY_ALIGNED(variable_name, size) \
  u8_t variable_name[size] __attribute__((section(".RxPoolSection"), aligned(32)))
#endif
LWIP_MEMPOOL_DECLARE(RX_POOL, ETH_RX_BUFFER_CNT, sizeof(RxBuff_t), "Zero-copy RX PBUF pool");

/* Variable Definitions */
static volatile uint8_t RxAllocStatus;

#if defined ( __ICCARM__ ) /*!< IAR Compiler */

#pragma location=0x2007c000
ETH_DMADescTypeDef  DMARxDscrTab[ETH_RX_DESC_CNT]; /* Ethernet Rx DMA Descriptors */
#pragma location=0x2007c0a0
ETH_DMADescTypeDef  DMATxDscrTab[ETH_TX_DESC_CNT]; /* Ethernet Tx DMA Descriptors */

#elif defined ( __CC_ARM )  /* MDK ARM Compiler */

__attribute__((at(0x2007c000))) ETH_DMADescTypeDef  DMARxDscrTab[ETH_RX_DESC_CNT]; /* Ethernet Rx DMA Descriptors */
__attribute__((at(0x2007c0a0))) ETH_DMADescTypeDef  DMATxDscrTab[ETH_TX_DESC_CNT]; /* Ethernet Tx DMA Descriptors */
__attribute__((at(0x2007c200))) uint8_t TxDmaBuff[ETH_TX_DESC_CNT][ETH_TX_DMA_BUFFER_SIZE] __ALIGNED(32);

#elif defined ( __ARMCC_VERSION )  /* ARMCLANG */

ETH_DMADescTypeDef DMARxDscrTab[ETH_RX_DESC_CNT]
  __attribute__((section(".ARM.__at_0x2007C000"))); /* Ethernet Rx DMA Descriptors */
ETH_DMADescTypeDef DMATxDscrTab[ETH_TX_DESC_CNT]
  __attribute__((section(".ARM.__at_0x2007C0A0"))); /* Ethernet Tx DMA Descriptors */
uint8_t TxDmaBuff[ETH_TX_DESC_CNT][ETH_TX_DMA_BUFFER_SIZE]
  __attribute__((section(".ARM.__at_0x2007C200"))) __ALIGNED(32);

#elif defined ( __GNUC__ ) /* GNU Compiler */

ETH_DMADescTypeDef DMARxDscrTab[ETH_RX_DESC_CNT] __attribute__((section(".RxDescripSection"))); /* Ethernet Rx DMA Descriptors */
ETH_DMADescTypeDef DMATxDscrTab[ETH_TX_DESC_CNT] __attribute__((section(".TxDescripSection")));   /* Ethernet Tx DMA Descriptors */
uint8_t TxDmaBuff[ETH_TX_DESC_CNT][ETH_TX_DMA_BUFFER_SIZE] __attribute__((section(".TxArraySection"))) __ALIGNED(32);

#endif

/* USER CODE BEGIN 2 */
static uint8_t g_ethernetif_mac_addr[ETH_HWADDR_LEN];

volatile uint32_t g_eth_tx_packets = 0U;
volatile uint32_t g_eth_tx_errors = 0U;
volatile uint32_t g_eth_rx_packets = 0U;
volatile uint32_t g_eth_rx_null_reads = 0U;
volatile uint32_t g_eth_rx_alloc_failures = 0U;
volatile uint32_t g_eth_rx_alloc_recoveries = 0U;
volatile uint32_t g_eth_rx_alloc_stalled = 0U;
volatile uint32_t g_eth_last_rx_ms = 0U;
volatile uint32_t g_eth_rx_self_packets = 0U;
volatile uint32_t g_eth_rx_arp_packets = 0U;
volatile uint32_t g_eth_rx_ipv4_packets = 0U;
volatile uint32_t g_eth_rx_udp_packets = 0U;
volatile uint32_t g_eth_rx_dhcp_like_packets = 0U;
volatile uint32_t g_eth_last_rx_ethertype = 0U;
volatile uint32_t g_eth_last_rx_ip_proto = 0U;
volatile uint32_t g_eth_last_rx_udp_src_port = 0U;
volatile uint32_t g_eth_last_rx_udp_dst_port = 0U;
volatile uint32_t g_eth_tx_ipv4_packets = 0U;
volatile uint32_t g_eth_tx_udp_packets = 0U;
volatile uint32_t g_eth_tx_dhcp_like_packets = 0U;
volatile uint32_t g_eth_last_tx_ethertype = 0U;
volatile uint32_t g_eth_last_tx_ip_proto = 0U;
volatile uint32_t g_eth_last_tx_udp_src_port = 0U;
volatile uint32_t g_eth_last_tx_udp_dst_port = 0U;
volatile uint32_t g_eth_last_tx_ip_src = 0U;
volatile uint32_t g_eth_last_tx_ip_dst = 0U;
volatile uint32_t g_eth_last_tx_dst_mac_0_1 = 0U;
volatile uint32_t g_eth_last_tx_dst_mac_2_5 = 0U;
volatile uint32_t g_eth_tx_free_packets = 0U;
volatile uint32_t g_eth_last_tx_hal_status = 0U;
volatile uint32_t g_eth_last_tx_hal_error = 0U;
volatile uint32_t g_eth_last_tx_dma_error = 0U;
volatile uint32_t g_eth_tx_reclaim_calls = 0U;
volatile uint32_t g_eth_tx_reclaim_schedule_errors = 0U;
volatile uint32_t g_eth_irq_count = 0U;
volatile uint32_t g_eth_rx_cplt_count = 0U;
volatile uint32_t g_eth_link_thread_started = 0U;
volatile uint32_t g_eth_link_thread_loops = 0U;
volatile int32_t g_eth_last_phy_link_state = 0;
volatile uint32_t g_eth_link_up_events = 0U;
volatile uint32_t g_eth_link_down_events = 0U;
volatile int32_t g_eth_phy_disable_power_status = 0;
volatile int32_t g_eth_phy_start_autoneg_status = 0;
volatile uint32_t g_eth_phy_bcr_after_cfg = 0U;
volatile uint32_t g_eth_phy_bsr_after_cfg = 0U;
volatile uint32_t g_eth_phy_physcsr_after_cfg = 0U;
volatile uint32_t g_eth_phy_retry_count = 0U;
volatile int32_t g_eth_phy_forced_mode = 0;
static uint32_t g_eth_tx_dma_index = 0U;
static volatile uint8_t g_eth_tx_reclaim_pending = 0U;
/* USER CODE END 2 */

osSemaphoreId RxPktSemaphore = NULL;   /* Semaphore to signal incoming packets */
osSemaphoreId TxPktSemaphore = NULL;   /* Semaphore to signal transmit packet complete */

/* Global Ethernet handle */
ETH_HandleTypeDef heth;
ETH_TxPacketConfig TxConfig;

/* Private function prototypes -----------------------------------------------*/
static void ethernetif_input(void const * argument);
static void RMII_Thread( void const * argument );
int32_t ETH_PHY_IO_Init(void);
int32_t ETH_PHY_IO_DeInit (void);
int32_t ETH_PHY_IO_ReadReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t *pRegVal);
int32_t ETH_PHY_IO_WriteReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t RegVal);
int32_t ETH_PHY_IO_GetTick(void);

lan8742_Object_t LAN8742;
lan8742_IOCtx_t  LAN8742_IOCtx = {ETH_PHY_IO_Init,
                                  ETH_PHY_IO_DeInit,
                                  ETH_PHY_IO_WriteReg,
                                  ETH_PHY_IO_ReadReg,
                                  ETH_PHY_IO_GetTick};

/* USER CODE BEGIN 3 */

/* USER CODE END 3 */

/* Private functions ---------------------------------------------------------*/
void pbuf_free_custom(struct pbuf *p);

/**
  * @brief  Ethernet Rx Transfer completed callback
  * @param  handlerEth: ETH handler
  * @retval None
  */
void HAL_ETH_RxCpltCallback(ETH_HandleTypeDef *handlerEth)
{
  g_eth_rx_cplt_count++;
  osSemaphoreRelease(RxPktSemaphore);
}
/**
  * @brief  Ethernet Tx Transfer completed callback
  * @param  handlerEth: ETH handler
  * @retval None
  */
void HAL_ETH_TxCpltCallback(ETH_HandleTypeDef *handlerEth)
{
  osSemaphoreRelease(TxPktSemaphore);
}
/**
  * @brief  Ethernet DMA transfer error callback
  * @param  handlerEth: ETH handler
  * @retval None
  */
void HAL_ETH_ErrorCallback(ETH_HandleTypeDef *handlerEth)
{
  if((HAL_ETH_GetDMAError(handlerEth) & ETH_DMASR_RBUS) == ETH_DMASR_RBUS)
  {
     osSemaphoreRelease(RxPktSemaphore);
  }
}

/* USER CODE BEGIN 4 */
static void ethernetif_build_mac_address(uint8_t *mac_addr)
{
  uint32_t uid0 = HAL_GetUIDw0();
  uint32_t uid1 = HAL_GetUIDw1();
  uint32_t uid2 = HAL_GetUIDw2();

  /* Use a unique locally administered unicast MAC derived from the STM32 UID. */
  mac_addr[0] = 0x02U;
  mac_addr[1] = (uint8_t)(uid2 >> 8);
  mac_addr[2] = (uint8_t)(uid2 >> 0);
  mac_addr[3] = (uint8_t)(uid1 >> 16);
  mac_addr[4] = (uint8_t)(uid0 >> 8);
  mac_addr[5] = (uint8_t)(uid0 >> 0);
}

static void ethernetif_relax_rx_filter(void)
{
  ETH_MACFilterConfigTypeDef filter_config;

  if (HAL_ETH_GetMACFilterConfig(&heth, &filter_config) == HAL_OK)
  {
    /*
     * During bring-up, prefer receiving too much over silently filtering
     * DHCP/ARP traffic before the DMA ever sees it.
     */
    filter_config.PromiscuousMode = DISABLE;
    filter_config.ReceiveAllMode = ENABLE;
    filter_config.HashMulticast = DISABLE;
    filter_config.PassAllMulticast = ENABLE;
    filter_config.BroadcastFilter = DISABLE;
    filter_config.HachOrPerfectFilter = DISABLE;
    filter_config.SrcAddrFiltering = DISABLE;
    filter_config.SrcAddrInverseFiltering = DISABLE;
    filter_config.DestAddrInverseFiltering = DISABLE;
    (void)HAL_ETH_SetMACFilterConfig(&heth, &filter_config);
  }
}

static void ethernetif_relax_rx_processing(void)
{
  ETH_MACConfigTypeDef mac_config;
  ETH_DMAConfigTypeDef dma_config;

  if (HAL_ETH_GetMACConfig(&heth, &mac_config) == HAL_OK)
  {
    /*
     * The current blocker is "offers visible on wire but never reaching lwIP".
     * Disable MAC-side RX checksum classification during bring-up so DHCP/UDP
     * broadcasts are not discarded before software can inspect them.
     */
    mac_config.ChecksumOffload = DISABLE;
    (void)HAL_ETH_SetMACConfig(&heth, &mac_config);
  }

  if (HAL_ETH_GetDMAConfig(&heth, &dma_config) == HAL_OK)
  {
    dma_config.DropTCPIPChecksumErrorFrame = DISABLE;
    dma_config.ForwardErrorFrames = ENABLE;
    dma_config.ForwardUndersizedGoodFrames = ENABLE;
    (void)HAL_ETH_SetDMAConfig(&heth, &dma_config);
  }
}

static void ethernetif_enable_multicast_rx(void)
{
#if LWIP_IGMP
  ETH_MACFilterConfigTypeDef filter_config;

  if (HAL_ETH_GetMACFilterConfig(&heth, &filter_config) == HAL_OK)
  {
    filter_config.PromiscuousMode = ENABLE;
    filter_config.ReceiveAllMode = ENABLE;
    filter_config.PassAllMulticast = ENABLE;
    filter_config.BroadcastFilter = DISABLE;
    filter_config.HashMulticast = DISABLE;
    (void)HAL_ETH_SetMACFilterConfig(&heth, &filter_config);
  }
#endif
}

static void ethernetif_force_phy_ready(void)
{
  uint32_t bcr = 0U;
  uint32_t bsr_after_cfg = 0U;
  uint32_t bcr_after_cfg = 0U;
  uint32_t phys_after_cfg = 0U;

  g_eth_phy_disable_power_status = LAN8742_DisablePowerDownMode(&LAN8742);
  g_eth_phy_start_autoneg_status = LAN8742_StartAutoNego(&LAN8742);

  if (HAL_ETH_ReadPHYRegister(&heth, LAN8742.DevAddr, LAN8742_BCR, &bcr) == HAL_OK)
  {
    bcr |= (uint32_t)(LAN8742_BCR_AUTONEGO_EN | LAN8742_BCR_RESTART_AUTONEGO);
    (void)HAL_ETH_WritePHYRegister(&heth, LAN8742.DevAddr, LAN8742_BCR, bcr);
  }

  HAL_Delay(50U);
  (void)HAL_ETH_ReadPHYRegister(&heth, LAN8742.DevAddr, LAN8742_BSR, &bsr_after_cfg);
  (void)HAL_ETH_ReadPHYRegister(&heth, LAN8742.DevAddr, LAN8742_BCR, &bcr_after_cfg);
  (void)HAL_ETH_ReadPHYRegister(&heth, LAN8742.DevAddr, LAN8742_PHYSCSR, &phys_after_cfg);
  g_eth_phy_bsr_after_cfg = bsr_after_cfg;
  g_eth_phy_bcr_after_cfg = bcr_after_cfg;
  g_eth_phy_physcsr_after_cfg = phys_after_cfg;
  g_eth_phy_forced_mode = 0;
}

static int32_t ethernetif_force_phy_link_mode(uint32_t link_state)
{
  int32_t status = LAN8742_STATUS_ERROR;

  status = LAN8742_SetLinkState(&LAN8742, link_state);
  g_eth_phy_forced_mode = (int32_t)link_state;
  return status;
}

/* USER CODE END 4 */

/*******************************************************************************
                       LL Driver Interface ( LwIP stack --> ETH)
*******************************************************************************/
/**
 * @brief In this function, the hardware should be initialized.
 * Called from ethernetif_init().
 *
 * @param netif the already initialized lwip network interface structure
 *        for this ethernetif
 */
static void low_level_init(struct netif *netif)
{
  HAL_StatusTypeDef hal_eth_init_status = HAL_OK;
  uint32_t duplex, speed = 0;
  int32_t PHYLinkState = 0;
  ETH_MACConfigTypeDef MACConf = {0};
  /* Start ETH HAL Init */

  heth.Instance = ETH;
  ethernetif_build_mac_address(g_ethernetif_mac_addr);
  heth.Init.MACAddr = g_ethernetif_mac_addr;
  heth.Init.MediaInterface = HAL_ETH_RMII_MODE;
  heth.Init.TxDesc = DMATxDscrTab;
  heth.Init.RxDesc = DMARxDscrTab;
  heth.Init.RxBuffLen = 1536;

  /* USER CODE BEGIN MACADDRESS */

  /* USER CODE END MACADDRESS */

  hal_eth_init_status = HAL_ETH_Init(&heth);
  ethernetif_relax_rx_filter();
  ethernetif_relax_rx_processing();

  memset(&TxConfig, 0 , sizeof(ETH_TxPacketConfig));
#if CHECKSUM_BY_HARDWARE
  TxConfig.Attributes = ETH_TX_PACKETS_FEATURES_CSUM | ETH_TX_PACKETS_FEATURES_CRCPAD;
  TxConfig.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC;
#else
  /*
   * HAL only updates the descriptor CIC field when the CSUM feature bit is set.
   * Keep the bit enabled here so recycled TX descriptors are explicitly forced
   * into checksum-bypass mode instead of inheriting a previous packet's setting.
   */
  TxConfig.Attributes = ETH_TX_PACKETS_FEATURES_CSUM | ETH_TX_PACKETS_FEATURES_CRCPAD;
  TxConfig.ChecksumCtrl = ETH_CHECKSUM_DISABLE;
#endif
  TxConfig.CRCPadCtrl = ETH_CRC_PAD_INSERT;

  /* End ETH HAL Init */

  /* Initialize the RX POOL */
  LWIP_MEMPOOL_INIT(RX_POOL);

#if LWIP_ARP || LWIP_ETHERNET

  /* set MAC hardware address length */
  netif->hwaddr_len = ETH_HWADDR_LEN;

  /* set MAC hardware address */
  netif->hwaddr[0] =  heth.Init.MACAddr[0];
  netif->hwaddr[1] =  heth.Init.MACAddr[1];
  netif->hwaddr[2] =  heth.Init.MACAddr[2];
  netif->hwaddr[3] =  heth.Init.MACAddr[3];
  netif->hwaddr[4] =  heth.Init.MACAddr[4];
  netif->hwaddr[5] =  heth.Init.MACAddr[5];

  /* maximum transfer unit */
  netif->mtu = ETH_MAX_PAYLOAD;

  /* Accept broadcast address and ARP traffic */
  /* don't set NETIF_FLAG_ETHARP if this device is not an ethernet one */
  #if LWIP_ARP
    netif->flags |= NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
  #else
    netif->flags |= NETIF_FLAG_BROADCAST;
  #endif /* LWIP_ARP */
  #if LWIP_IGMP
    netif->flags |= NETIF_FLAG_IGMP;
    ethernetif_enable_multicast_rx();
  #endif /* LWIP_IGMP */

  /* create a binary semaphore used for informing ethernetif of frame reception */
  osSemaphoreDef(RxSem);
  RxPktSemaphore = osSemaphoreCreate(osSemaphore(RxSem), 1);

  /* create a binary semaphore used for informing ethernetif of frame transmission */
  osSemaphoreDef(TxSem);
  TxPktSemaphore = osSemaphoreCreate(osSemaphore(TxSem), 1);

  /* Decrease the semaphore's initial count from 1 to 0 */
  osSemaphoreWait(RxPktSemaphore, 0);
  osSemaphoreWait(TxPktSemaphore, 0);

  /* create the task that handles the ETH_MAC */
/* USER CODE BEGIN OS_THREAD_DEF_CREATE_CMSIS_RTOS_V1 */
  osThreadDef(EthIf, ethernetif_input, osPriorityNormal, 0, INTERFACE_THREAD_STACK_SIZE);
  osThreadCreate (osThread(EthIf), netif);
/* USER CODE END OS_THREAD_DEF_CREATE_CMSIS_RTOS_V1 */

/* USER CODE BEGIN PHY_PRE_CONFIG */

/* USER CODE END PHY_PRE_CONFIG */
  /* Set PHY IO functions */
  LAN8742_RegisterBusIO(&LAN8742, &LAN8742_IOCtx);

  /* Initialize the LAN8742 ETH PHY */
  if(LAN8742_Init(&LAN8742) != LAN8742_STATUS_OK)
  {
    netif_set_link_down(netif);
    netif_set_down(netif);
    return;
  }

  /*
   * CubeMX only discovers the PHY address here. After recent regeneration the PHY
   * occasionally comes up with BCR=0x0000, i.e. autoneg disabled, which leaves
   * the stack permanently stuck at LINK_DOWN. Explicitly bring the PHY into the
   * expected state before the first link-state probe.
   */
  ethernetif_force_phy_ready();

  if (hal_eth_init_status == HAL_OK)
  {
    PHYLinkState = LAN8742_GetLinkState(&LAN8742);
    g_eth_last_phy_link_state = PHYLinkState;

    /* Get link state */
    if(PHYLinkState <= LAN8742_STATUS_LINK_DOWN)
    {
      netif_set_link_down(netif);
      netif_set_down(netif);
    }
    else
    {
      switch (PHYLinkState)
      {
      case LAN8742_STATUS_100MBITS_FULLDUPLEX:
        duplex = ETH_FULLDUPLEX_MODE;
        speed = ETH_SPEED_100M;
        break;
      case LAN8742_STATUS_100MBITS_HALFDUPLEX:
        duplex = ETH_HALFDUPLEX_MODE;
        speed = ETH_SPEED_100M;
        break;
      case LAN8742_STATUS_10MBITS_FULLDUPLEX:
        duplex = ETH_FULLDUPLEX_MODE;
        speed = ETH_SPEED_10M;
        break;
      case LAN8742_STATUS_10MBITS_HALFDUPLEX:
        duplex = ETH_HALFDUPLEX_MODE;
        speed = ETH_SPEED_10M;
        break;
      default:
        duplex = ETH_FULLDUPLEX_MODE;
        speed = ETH_SPEED_100M;
        break;
      }

    /* Get MAC Config MAC */
    HAL_ETH_GetMACConfig(&heth, &MACConf);
    MACConf.DuplexMode = duplex;
    MACConf.Speed = speed;
    HAL_ETH_SetMACConfig(&heth, &MACConf);

    HAL_ETH_Start(&heth);
    if (tcpip_try_callback(ethernetif_netif_up_callback, netif) != ERR_OK)
    {
      ethernetif_uart_write("[net] low init netif up err\r\n");
    }

/* USER CODE BEGIN PHY_POST_CONFIG */

/* USER CODE END PHY_POST_CONFIG */
    }

  }
  else
  {
    Error_Handler();
  }
#endif /* LWIP_ARP || LWIP_ETHERNET */

/* USER CODE BEGIN LOW_LEVEL_INIT */

/* USER CODE END LOW_LEVEL_INIT */

  if(HAL_GetREVID() == 0x1000)
  {
    /*
      This thread will keep resetting the RMII interface until good frames are received
    */
    osThreadDef(RMII_Watchdog, RMII_Thread, osPriorityBelowNormal, 0, configMINIMAL_STACK_SIZE);
    osThreadCreate (osThread(RMII_Watchdog), NULL);
  }
}

/**
 * @brief This function should do the actual transmission of the packet. The packet is
 * contained in the pbuf that is passed to the function. This pbuf
 * might be chained.
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @param p the MAC packet to send (e.g. IP packet including MAC addresses and type)
 * @return ERR_OK if the packet could be sent
 *         an err_t value if the packet couldn't be sent
 *
 * @note Returning ERR_MEM here if a DMA queue of your MAC is full can lead to
 *       strange results. You might consider waiting for space in the DMA queue
 *       to become available since the stack doesn't retry to send a packet
 *       dropped because of memory failure (except for the TCP timers).
 */

static err_t low_level_output(struct netif *netif, struct pbuf *p)
{
  uint32_t copy_offset = 0U;
  uint32_t tx_slot = g_eth_tx_dma_index;
  struct pbuf *q = NULL;
  err_t errval = ERR_OK;
  HAL_StatusTypeDef tx_status;
  ETH_BufferTypeDef Txbuffer[ETH_TX_DESC_CNT] = {0};
  uint8_t *tx_dma_buffer = TxDmaBuff[tx_slot];

  LWIP_UNUSED_ARG(netif);
  memset(Txbuffer, 0, sizeof(Txbuffer));

  /*
   * HAL keeps completed descriptors reserved until ReleaseTxPacket runs.
   * Reclaim here before preparing another packet; low_level_output and the
   * scheduled reclaim callback both execute serially in tcpip_thread.
   */
  (void)HAL_ETH_ReleaseTxPacket(&heth);
  g_eth_tx_reclaim_calls++;

  if (p->tot_len > ETH_TX_DMA_BUFFER_SIZE)
  {
    g_eth_tx_errors++;
    return ERR_IF;
  }

  for (q = p; q != NULL; q = q->next)
  {
    ethernetif_copy_bytes_safe(&tx_dma_buffer[copy_offset], q->payload, q->len);
    copy_offset += q->len;
  }

  /* Ensure CPU-written TX data is visible to the ETH DMA on Cortex-M7 D-Cache. */
  SCB_CleanDCache_by_Addr((uint32_t *)tx_dma_buffer, (int32_t)((p->tot_len + 31U) & ~31U));

  Txbuffer[0].buffer = tx_dma_buffer;
  Txbuffer[0].len = p->tot_len;
  Txbuffer[0].next = NULL;

  TxConfig.Length = p->tot_len;
  TxConfig.TxBuffer = Txbuffer;
  TxConfig.pData = p;

  /* Keep using the known-good blocking TX path for now.
     HAL_ETH_ReleaseTxPacket() depends on CurrentPacketAddress being set. */
  pbuf_ref(p);
  heth.TxDescList.CurrentPacketAddress = (uint32_t *)p;
  tx_status = HAL_ETH_Transmit(&heth, &TxConfig, ETH_DMA_TRANSMIT_TIMEOUT);
  g_eth_last_tx_hal_status = (uint32_t)tx_status;
  g_eth_last_tx_hal_error = HAL_ETH_GetError(&heth);
  g_eth_last_tx_dma_error = HAL_ETH_GetDMAError(&heth);

  if (tx_status == HAL_OK)
  {
    g_eth_tx_packets++;
    g_eth_tx_dma_index = (tx_slot + 1U) % ETH_TX_DESC_CNT;
  }
  else
  {
    pbuf_free(p);
    g_eth_tx_errors++;
    errval = ERR_IF;
  }

  return errval;
}

static void ethernetif_tx_reclaim_callback(void *ctx)
{
  LWIP_UNUSED_ARG(ctx);

  (void)HAL_ETH_ReleaseTxPacket(&heth);
  g_eth_tx_reclaim_calls++;
  g_eth_tx_reclaim_pending = 0U;
}

void ethernetif_poll_tx_reclaim(void)
{
  if ((g_eth_tx_packets == g_eth_tx_free_packets) ||
      (g_eth_tx_reclaim_pending != 0U))
  {
    return;
  }

  g_eth_tx_reclaim_pending = 1U;
  if (tcpip_try_callback(ethernetif_tx_reclaim_callback, NULL) != ERR_OK)
  {
    g_eth_tx_reclaim_pending = 0U;
    g_eth_tx_reclaim_schedule_errors++;
  }
}

/**
 * @brief Should allocate a pbuf and transfer the bytes of the incoming
 * packet from the interface into the pbuf.
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @return a pbuf filled with the received packet (including MAC header)
 *         NULL on memory error
   */
static struct pbuf * low_level_input(struct netif *netif)
{
  struct pbuf *p = NULL;

  /*
   * Always let the HAL retry descriptor rebuilding. Gating this call on
   * RxAllocStatus can permanently stop RX if the pool-recovery notification
   * races with the input task.
   */
  HAL_ETH_ReadData(&heth, (void **)&p);

  if (p != NULL)
  {
    g_eth_rx_packets++;
    g_eth_last_rx_ms = HAL_GetTick();
  }
  else
  {
    g_eth_rx_null_reads++;
  }

  return p;
}

/**
 * @brief This function should be called when a packet is ready to be read
 * from the interface. It uses the function low_level_input() that
 * should handle the actual reception of bytes from the network
 * interface. Then the type of the received packet is determined and
 * the appropriate input function is called.
 *
 * @param netif the lwip network interface structure for this ethernetif
 */
static void ethernetif_input(void const * argument)
{
  struct pbuf *p = NULL;
  struct netif *netif = (struct netif *) argument;

  for( ;; )
  {
    if (osSemaphoreWait(RxPktSemaphore, TIME_WAITING_FOR_INPUT_POLL_MS) == osOK)
    {
    }

    do
    {
      p = low_level_input( netif );
      if (p != NULL)
      {
        if (netif->input( p, netif) != ERR_OK )
        {
          pbuf_free(p);
        }
      }
    } while(p!=NULL);

    osDelay(1);
  }
}

#if !LWIP_ARP
/**
 * This function has to be completed by user in case of ARP OFF.
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @return ERR_OK if ...
 */
static err_t low_level_output_arp_off(struct netif *netif, struct pbuf *q, const ip4_addr_t *ipaddr)
{
  err_t errval;
  errval = ERR_OK;

/* USER CODE BEGIN 5 */

/* USER CODE END 5 */

  return errval;

}
#endif /* LWIP_ARP */

/**
 * @brief Should be called at the beginning of the program to set up the
 * network interface. It calls the function low_level_init() to do the
 * actual setup of the hardware.
 *
 * This function should be passed as a parameter to netif_add().
 *
 * @param netif the lwip network interface structure for this ethernetif
 * @return ERR_OK if the loopif is initialized
 *         ERR_MEM if private data couldn't be allocated
 *         any other err_t on error
 */
err_t ethernetif_init(struct netif *netif)
{
  LWIP_ASSERT("netif != NULL", (netif != NULL));

#if LWIP_NETIF_HOSTNAME
  /* Initialize interface hostname */
  netif->hostname = "lwip";
#endif /* LWIP_NETIF_HOSTNAME */

  /*
   * Initialize the snmp variables and counters inside the struct netif.
   * The last argument should be replaced with your link speed, in units
   * of bits per second.
   */
  // MIB2_INIT_NETIF(netif, snmp_ifType_ethernet_csmacd, LINK_SPEED_OF_YOUR_NETIF_IN_BPS);

  netif->name[0] = IFNAME0;
  netif->name[1] = IFNAME1;
  /* We directly use etharp_output() here to save a function call.
   * You can instead declare your own function an call etharp_output()
   * from it if you have to do some checks before sending (e.g. if link
   * is available...) */

#if LWIP_IPV4
#if LWIP_ARP || LWIP_ETHERNET
#if LWIP_ARP
  netif->output = etharp_output;
#else
  /* The user should write its own code in low_level_output_arp_off function */
  netif->output = low_level_output_arp_off;
#endif /* LWIP_ARP */
#endif /* LWIP_ARP || LWIP_ETHERNET */
#endif /* LWIP_IPV4 */

#if LWIP_IPV6
  netif->output_ip6 = ethip6_output;
#endif /* LWIP_IPV6 */

  netif->linkoutput = low_level_output;

  /* initialize the hardware */
  low_level_init(netif);

  return ERR_OK;
}

/**
  * @brief  Custom Rx pbuf free callback
  * @param  pbuf: pbuf to be freed
  * @retval None
  */
void pbuf_free_custom(struct pbuf *p)
{
  struct pbuf_custom* custom_pbuf = (struct pbuf_custom*)p;
  LWIP_MEMPOOL_FREE(RX_POOL, custom_pbuf);

  /* If the Rx Buffer Pool was exhausted, signal the ethernetif_input task to
   * call HAL_ETH_GetRxDataBuffer to rebuild the Rx descriptors. */

  if (RxAllocStatus == RX_ALLOC_ERROR)
  {
    osSemaphoreRelease(RxPktSemaphore);
  }
}

/* USER CODE BEGIN 6 */

/**
* @brief  Returns the current time in milliseconds
*         when LWIP_TIMERS == 1 and NO_SYS == 1
* @param  None
* @retval Current Time value
*/
u32_t sys_now(void)
{
  return HAL_GetTick();
}

/* USER CODE END 6 */

/**
  * @brief  Initializes the ETH MSP.
  * @param  ethHandle: ETH handle
  * @retval None
  */

void HAL_ETH_MspInit(ETH_HandleTypeDef* ethHandle)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  if(ethHandle->Instance==ETH)
  {
  /* USER CODE BEGIN ETH_MspInit 0 */

  /* USER CODE END ETH_MspInit 0 */
    /* Enable Peripheral clock */
    __HAL_RCC_ETH_CLK_ENABLE();

    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
    /**ETH GPIO Configuration
    PC1     ------> ETH_MDC
    PA1     ------> ETH_REF_CLK
    PA2     ------> ETH_MDIO
    PA7     ------> ETH_CRS_DV
    PC4     ------> ETH_RXD0
    PC5     ------> ETH_RXD1
    PB13     ------> ETH_TXD1
    PG11     ------> ETH_TX_EN
    PG13     ------> ETH_TXD0
    */
    GPIO_InitStruct.Pin = GPIO_PIN_1|GPIO_PIN_4|GPIO_PIN_5;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF11_ETH;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF11_ETH;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_13;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF11_ETH;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_11|GPIO_PIN_13;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF11_ETH;
    HAL_GPIO_Init(GPIOG, &GPIO_InitStruct);

    /* Peripheral interrupt init */
    HAL_NVIC_SetPriority(ETH_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(ETH_IRQn);
  /* USER CODE BEGIN ETH_MspInit 1 */

  /* USER CODE END ETH_MspInit 1 */
  }
}

void HAL_ETH_MspDeInit(ETH_HandleTypeDef* ethHandle)
{
  if(ethHandle->Instance==ETH)
  {
  /* USER CODE BEGIN ETH_MspDeInit 0 */

  /* USER CODE END ETH_MspDeInit 0 */
    /* Peripheral clock disable */
    __HAL_RCC_ETH_CLK_DISABLE();

    /**ETH GPIO Configuration
    PC1     ------> ETH_MDC
    PA1     ------> ETH_REF_CLK
    PA2     ------> ETH_MDIO
    PA7     ------> ETH_CRS_DV
    PC4     ------> ETH_RXD0
    PC5     ------> ETH_RXD1
    PB13     ------> ETH_TXD1
    PG11     ------> ETH_TX_EN
    PG13     ------> ETH_TXD0
    */
    HAL_GPIO_DeInit(GPIOC, GPIO_PIN_1|GPIO_PIN_4|GPIO_PIN_5);

    HAL_GPIO_DeInit(GPIOA, GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_7);

    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_13);

    HAL_GPIO_DeInit(GPIOG, GPIO_PIN_11|GPIO_PIN_13);

    /* Peripheral interrupt Deinit*/
    HAL_NVIC_DisableIRQ(ETH_IRQn);

  /* USER CODE BEGIN ETH_MspDeInit 1 */

  /* USER CODE END ETH_MspDeInit 1 */
  }
}

/*******************************************************************************
                       PHI IO Functions
*******************************************************************************/
/**
  * @brief  Initializes the MDIO interface GPIO and clocks.
  * @param  None
  * @retval 0 if OK, -1 if ERROR
  */
int32_t ETH_PHY_IO_Init(void)
{
  /* We assume that MDIO GPIO configuration is already done
     in the ETH_MspInit() else it should be done here
  */

  /* Configure the MDIO Clock */
  HAL_ETH_SetMDIOClockRange(&heth);

  return 0;
}

/**
  * @brief  De-Initializes the MDIO interface .
  * @param  None
  * @retval 0 if OK, -1 if ERROR
  */
int32_t ETH_PHY_IO_DeInit (void)
{
  return 0;
}

/**
  * @brief  Read a PHY register through the MDIO interface.
  * @param  DevAddr: PHY port address
  * @param  RegAddr: PHY register address
  * @param  pRegVal: pointer to hold the register value
  * @retval 0 if OK -1 if Error
  */
int32_t ETH_PHY_IO_ReadReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t *pRegVal)
{
  if(HAL_ETH_ReadPHYRegister(&heth, DevAddr, RegAddr, pRegVal) != HAL_OK)
  {
    return -1;
  }

  return 0;
}

/**
  * @brief  Write a value to a PHY register through the MDIO interface.
  * @param  DevAddr: PHY port address
  * @param  RegAddr: PHY register address
  * @param  RegVal: Value to be written
  * @retval 0 if OK -1 if Error
  */
int32_t ETH_PHY_IO_WriteReg(uint32_t DevAddr, uint32_t RegAddr, uint32_t RegVal)
{
  if(HAL_ETH_WritePHYRegister(&heth, DevAddr, RegAddr, RegVal) != HAL_OK)
  {
    return -1;
  }

  return 0;
}

/**
  * @brief  Get the time in millisecons used for internal PHY driver process.
  * @retval Time value
  */
int32_t ETH_PHY_IO_GetTick(void)
{
  return HAL_GetTick();
}

/**
  * @brief  Check the ETH link state then update ETH driver and netif link accordingly.
  * @retval None
  */

void ethernet_link_thread(void const * argument)
{
  ETH_MACConfigTypeDef MACConf = {0};
  int32_t PHYLinkState = 0;
  uint32_t linkchanged = 0U, speed = 0U, duplex = 0U;
  uint8_t first_loop_logged = 0U;
  uint32_t link_down_loops = 0U;
  uint32_t force_mode_hold_loops = 0U;

  struct netif *netif = (struct netif *) argument;
/* USER CODE BEGIN ETH link init */
  g_eth_link_thread_started = 1U;
/* USER CODE END ETH link init */

  for(;;)
  {
  g_eth_link_thread_loops++;
  PHYLinkState = LAN8742_GetLinkState(&LAN8742);
  g_eth_last_phy_link_state = PHYLinkState;
  linkchanged = 0U;

  if (first_loop_logged == 0U)
  {
    first_loop_logged = 1U;
  }

    if(netif_is_link_up(netif) && (PHYLinkState <= LAN8742_STATUS_LINK_DOWN))
    {
      HAL_ETH_Stop(&heth);
      if (tcpip_try_callback(ethernetif_netif_down_callback, netif) != ERR_OK)
      {
        ethernetif_uart_write("[net] ethlink netif down err\r\n");
      }
      g_eth_link_down_events++;
      link_down_loops = 0U;
      force_mode_hold_loops = 0U;
    }
    else if(!netif_is_link_up(netif) && (PHYLinkState > LAN8742_STATUS_LINK_DOWN))
    {
      switch (PHYLinkState)
      {
    case LAN8742_STATUS_100MBITS_FULLDUPLEX:
      duplex = ETH_FULLDUPLEX_MODE;
      speed = ETH_SPEED_100M;
      linkchanged = 1;
      break;
    case LAN8742_STATUS_100MBITS_HALFDUPLEX:
      duplex = ETH_HALFDUPLEX_MODE;
      speed = ETH_SPEED_100M;
      linkchanged = 1;
      break;
    case LAN8742_STATUS_10MBITS_FULLDUPLEX:
      duplex = ETH_FULLDUPLEX_MODE;
      speed = ETH_SPEED_10M;
      linkchanged = 1;
      break;
    case LAN8742_STATUS_10MBITS_HALFDUPLEX:
      duplex = ETH_HALFDUPLEX_MODE;
      speed = ETH_SPEED_10M;
      linkchanged = 1;
      break;
    default:
      break;
    }

      if(linkchanged)
      {
        /* Get MAC Config MAC */
        HAL_ETH_GetMACConfig(&heth, &MACConf);
        MACConf.DuplexMode = duplex;
        MACConf.Speed = speed;
        HAL_ETH_SetMACConfig(&heth, &MACConf);
        HAL_ETH_Start(&heth);
        if (tcpip_try_callback(ethernetif_netif_up_callback, netif) == ERR_OK)
        {
          g_eth_link_up_events++;
          g_eth_phy_retry_count = 0U;
          link_down_loops = 0U;
          force_mode_hold_loops = 0U;
        }
        else
        {
          ethernetif_uart_write("[net] ethlink netif link up err\r\n");
        }
      }
    }
    else if (PHYLinkState <= LAN8742_STATUS_LINK_DOWN)
    {
      link_down_loops++;

      if ((link_down_loops % 10U) == 0U)
      {
        if (force_mode_hold_loops != 0U)
        {
          if (force_mode_hold_loops > 10U)
          {
            force_mode_hold_loops -= 10U;
          }
          else
          {
            force_mode_hold_loops = 0U;
          }
        }
        else
        {
          g_eth_phy_retry_count++;

          if (g_eth_phy_retry_count <= 3U)
          {
            ethernetif_force_phy_ready();
          }
          else
          {
            if (ethernetif_force_phy_link_mode(LAN8742_STATUS_100MBITS_HALFDUPLEX) == LAN8742_STATUS_OK)
            {
              force_mode_hold_loops = 30U;
            }
            else
            {
              ethernetif_force_phy_ready();
            }
          }
        }
      }
    }
    else
    {
      link_down_loops = 0U;
    }

  /* USER CODE BEGIN ETH link Thread core code for User BSP */

/* USER CODE END ETH link Thread core code for User BSP */

    osDelay(100);
  }
}

void HAL_ETH_RxAllocateCallback(uint8_t **buff)
{
  /* USER CODE BEGIN HAL ETH RxAllocateCallback */
  struct pbuf_custom *p = LWIP_MEMPOOL_ALLOC(RX_POOL);
  if (p)
  {
      if (RxAllocStatus == RX_ALLOC_ERROR)
      {
        g_eth_rx_alloc_recoveries++;
      }
      RxAllocStatus = RX_ALLOC_OK;
      g_eth_rx_alloc_stalled = 0U;
      /* Get the buff from the struct pbuf address. */
      *buff = (uint8_t *)p + offsetof(RxBuff_t, buff);
      p->custom_free_function = pbuf_free_custom;
      /* Initialize the struct pbuf.
      * This must be performed whenever a buffer's allocated because it may be
      * changed by lwIP or the app, e.g., pbuf_free decrements ref. */
      ethernetif_init_custom_pbuf_safe(p, *buff, 0U);
    }
    else
    {
      if (RxAllocStatus != RX_ALLOC_ERROR)
      {
        g_eth_rx_alloc_failures++;
      }
      RxAllocStatus = RX_ALLOC_ERROR;
      g_eth_rx_alloc_stalled = 1U;
      *buff = NULL;
    }
/* USER CODE END HAL ETH RxAllocateCallback */
}

void HAL_ETH_RxLinkCallback(void **pStart, void **pEnd, uint8_t *buff, uint16_t Length)
{
/* USER CODE BEGIN HAL ETH RxLinkCallback */

  struct pbuf **ppStart = (struct pbuf **)pStart;
  struct pbuf **ppEnd = (struct pbuf **)pEnd;
  struct pbuf *p = NULL;

  /* Get the struct pbuf from the buff address. */
  p = (struct pbuf *)(buff - offsetof(RxBuff_t, buff));
  p->next = NULL;
  p->tot_len = 0;
  p->len = Length;

  /* Chain the buffer. */
  if (!*ppStart)
  {
    /* The first buffer of the packet. */
    *ppStart = p;
  }
  else
  {
    /* Chain the buffer to the end of the packet. */
    (*ppEnd)->next = p;
  }
  *ppEnd  = p;

  /* Update the total length of all the buffers of the chain. Each pbuf in the chain should have its tot_len
   * set to its own length, plus the length of all the following pbufs in the chain. */
  for (p = *ppStart; p != NULL; p = p->next)
  {
    p->tot_len += Length;
  }

  /* Invalidate data cache because Rx DMA's writing to physical memory makes it stale. */
  SCB_InvalidateDCache_by_Addr((uint32_t *)buff, Length);

/* USER CODE END HAL ETH RxLinkCallback */
}

void HAL_ETH_TxFreeCallback(uint32_t * buff)
{
/* USER CODE BEGIN HAL ETH TxFreeCallback */
  g_eth_tx_free_packets++;
  pbuf_free((struct pbuf *)buff);

/* USER CODE END HAL ETH TxFreeCallback */
}

/* USER CODE BEGIN 8 */

/* USER CODE END 8 */

void RMII_Thread( void const * argument )
{
  (void) argument;

  for(;;)
  {
    /* some unicast good packets are received */
    if(heth.Instance->MMCRGUFCR > 0U)
    {
      /* RMII Init is OK: Delete the Thread */
      osThreadTerminate(NULL);
    }
    else if(heth.Instance->MMCRFCECR > 10U)
    {
      /* ETH received too many packets with CRC errors, resetting RMII */
      SYSCFG->PMC &= ~SYSCFG_PMC_MII_RMII_SEL;
      SYSCFG->PMC |= SYSCFG_PMC_MII_RMII_SEL;

      heth.Instance->MMCCR |= ETH_MMCCR_CR;
      osDelay(200);
    }
    else
    {
      /* Delay 200 ms */
      osDelay(200);
    }
  }
}
