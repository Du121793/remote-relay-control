#include "fault_service.h"
#include "../Config/rtc_backup_layout.h"

#include "main.h"

#include <string.h>

#define FAULT_SERVICE_SIGNATURE 0x46544C54UL

static void fault_service_prepare_backup_domain(void)
{
  __HAL_RCC_PWR_CLK_ENABLE();
  HAL_PWR_EnableBkUpAccess();
}

/* Store fault summary (6 registers: DR0–DR5 per rtc_backup_layout.h).
   Full register dump is printed to UART at crash time; backup only
   keeps the minimum needed for post-mortem diagnosis. */
static void fault_service_store_record(const fault_service_record_t *record)
{
  if (record == NULL) return;
  fault_service_prepare_backup_domain();
  /* Clear signature first so partial write is detected as invalid */
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_FAULT_SIGNATURE, 0U);
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_FAULT_TYPE,      record->type);
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_FAULT_PC,        record->pc);
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_FAULT_LR,        record->lr);
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_FAULT_CFSR,      record->cfsr);
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_FAULT_HFSR,      record->hfsr);
  /* Write signature last so valid signature means complete record */
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_FAULT_SIGNATURE, FAULT_SERVICE_SIGNATURE);
}

void fault_service_record_hardfault(uint32_t *stack, uint32_t exc_lr)
{
  fault_service_record_t record;

  record.valid = 1U;
  record.type = FAULT_SERVICE_TYPE_HARDFAULT;
  record.cfsr = SCB->CFSR;
  record.hfsr = SCB->HFSR;
  record.mmfar = SCB->MMFAR;
  record.bfar = SCB->BFAR;
  record.exc_lr = exc_lr;
  record.sp = (uint32_t)stack;
  record.pc = (stack != NULL) ? stack[6] : 0U;
  record.lr = (stack != NULL) ? stack[5] : 0U;
  record.psr = (stack != NULL) ? stack[7] : 0U;
  record.r0 = (stack != NULL) ? stack[0] : 0U;
  record.r1 = (stack != NULL) ? stack[1] : 0U;
  record.r2 = (stack != NULL) ? stack[2] : 0U;
  record.r3 = (stack != NULL) ? stack[3] : 0U;
  record.r12 = (stack != NULL) ? stack[4] : 0U;
  fault_service_store_record(&record);
}

void fault_service_clear_record(void)
{
  fault_service_prepare_backup_domain();
  HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_FAULT_SIGNATURE, 0U);
}

uint8_t fault_service_load_record(fault_service_record_t *record)
{
  uint32_t sig, type, pc, lr, cfsr, hfsr;

  if (record == NULL) return 0U;
  fault_service_prepare_backup_domain();

  sig  = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_FAULT_SIGNATURE);
  if (sig != FAULT_SERVICE_SIGNATURE) return 0U;

  type = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_FAULT_TYPE);
  pc   = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_FAULT_PC);
  lr   = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_FAULT_LR);
  cfsr = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_FAULT_CFSR);
  hfsr = HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_FAULT_HFSR);

  memset(record, 0, sizeof(*record));
  record->valid = 1U;
  record->type  = (uint8_t)type;
  record->pc    = pc;
  record->lr    = lr;
  record->cfsr  = cfsr;
  record->hfsr  = hfsr;
  return 1U;
}
