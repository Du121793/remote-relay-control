#include "fault_service.h"

#include "main.h"

#include <string.h>

#define FAULT_SERVICE_SIGNATURE 0x46544C54UL
#define FAULT_SERVICE_REG_COUNT 16U

static void fault_service_prepare_backup_domain(void)
{
  __HAL_RCC_PWR_CLK_ENABLE();
  HAL_PWR_EnableBkUpAccess();
}

static void fault_service_write_word(uint32_t index, uint32_t value)
{
  volatile uint32_t *base = &RTC->BKP0R;
  base[index] = value;
}

static uint32_t fault_service_read_word(uint32_t index)
{
  volatile uint32_t *base = &RTC->BKP0R;
  return base[index];
}

static void fault_service_store_record(const fault_service_record_t *record)
{
  if (record == NULL)
  {
    return;
  }

  fault_service_prepare_backup_domain();
  fault_service_write_word(0U, FAULT_SERVICE_SIGNATURE);
  fault_service_write_word(1U, record->type);
  fault_service_write_word(2U, record->cfsr);
  fault_service_write_word(3U, record->hfsr);
  fault_service_write_word(4U, record->mmfar);
  fault_service_write_word(5U, record->bfar);
  fault_service_write_word(6U, record->exc_lr);
  fault_service_write_word(7U, record->sp);
  fault_service_write_word(8U, record->pc);
  fault_service_write_word(9U, record->lr);
  fault_service_write_word(10U, record->psr);
  fault_service_write_word(11U, record->r0);
  fault_service_write_word(12U, record->r1);
  fault_service_write_word(13U, record->r2);
  fault_service_write_word(14U, record->r3);
  fault_service_write_word(15U, record->r12);
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

void fault_service_record_simple(fault_service_type_t type)
{
  fault_service_record_t record;

  memset(&record, 0, sizeof(record));
  record.valid = 1U;
  record.type = (uint32_t)type;
  record.cfsr = SCB->CFSR;
  record.hfsr = SCB->HFSR;
  record.mmfar = SCB->MMFAR;
  record.bfar = SCB->BFAR;
  fault_service_store_record(&record);
}

uint8_t fault_service_peek(fault_service_record_t *record)
{
  if (record == NULL)
  {
    return 0U;
  }

  fault_service_prepare_backup_domain();
  if (fault_service_read_word(0U) != FAULT_SERVICE_SIGNATURE)
  {
    memset(record, 0, sizeof(*record));
    return 0U;
  }

  memset(record, 0, sizeof(*record));
  record->valid = 1U;
  record->type = fault_service_read_word(1U);
  record->cfsr = fault_service_read_word(2U);
  record->hfsr = fault_service_read_word(3U);
  record->mmfar = fault_service_read_word(4U);
  record->bfar = fault_service_read_word(5U);
  record->exc_lr = fault_service_read_word(6U);
  record->sp = fault_service_read_word(7U);
  record->pc = fault_service_read_word(8U);
  record->lr = fault_service_read_word(9U);
  record->psr = fault_service_read_word(10U);
  record->r0 = fault_service_read_word(11U);
  record->r1 = fault_service_read_word(12U);
  record->r2 = fault_service_read_word(13U);
  record->r3 = fault_service_read_word(14U);
  record->r12 = fault_service_read_word(15U);
  return 1U;
}

void fault_service_clear(void)
{
  uint32_t index;

  fault_service_prepare_backup_domain();
  for (index = 0U; index < FAULT_SERVICE_REG_COUNT; ++index)
  {
    fault_service_write_word(index, 0U);
  }
}
