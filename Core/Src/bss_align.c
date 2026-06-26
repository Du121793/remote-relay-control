/* FreeRTOS heap placed in DTCM (0x20000000, 128KB CPU-only RAM).
   DTCM is physically separate from SRAM1 — DMA cannot touch it,
   eliminating the lwIP-heap / ETH-DMA / stack overlap that caused
   HardFault crashes in PendSV at 0x08005E86.
   Size must match configTOTAL_HEAP_SIZE in FreeRTOSConfig.h (61440). */
#include <stdint.h>

uint8_t ucHeap[61440]
  __attribute__((section(".ARM.__at_0x20000000"), aligned(8)));
