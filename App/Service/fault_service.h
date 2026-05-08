#ifndef FAULT_SERVICE_H
#define FAULT_SERVICE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
  FAULT_SERVICE_TYPE_NONE = 0,
  FAULT_SERVICE_TYPE_HARDFAULT = 1,
  FAULT_SERVICE_TYPE_MEMMANAGE = 2,
  FAULT_SERVICE_TYPE_BUSFAULT = 3,
  FAULT_SERVICE_TYPE_USAGEFAULT = 4
} fault_service_type_t;

typedef struct
{
  uint32_t valid;
  uint32_t type;
  uint32_t cfsr;
  uint32_t hfsr;
  uint32_t mmfar;
  uint32_t bfar;
  uint32_t exc_lr;
  uint32_t sp;
  uint32_t pc;
  uint32_t lr;
  uint32_t psr;
  uint32_t r0;
  uint32_t r1;
  uint32_t r2;
  uint32_t r3;
  uint32_t r12;
} fault_service_record_t;

void fault_service_record_hardfault(uint32_t *stack, uint32_t exc_lr);
void fault_service_record_simple(fault_service_type_t type);
uint8_t fault_service_peek(fault_service_record_t *record);
void fault_service_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* FAULT_SERVICE_H */
