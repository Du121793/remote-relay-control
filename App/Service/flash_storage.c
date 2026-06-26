#include "flash_storage.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

static StaticSemaphore_t g_flash_storage_mutex_buffer;
static SemaphoreHandle_t g_flash_storage_mutex = NULL;

void flash_storage_init(void)
{
  if (g_flash_storage_mutex == NULL)
  {
    g_flash_storage_mutex =
      xSemaphoreCreateMutexStatic(&g_flash_storage_mutex_buffer);
  }
}

void flash_storage_lock(void)
{
  flash_storage_init();
  if (g_flash_storage_mutex != NULL &&
      xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
  {
    (void)xSemaphoreTake(g_flash_storage_mutex, portMAX_DELAY);
  }
}

void flash_storage_unlock(void)
{
  if (g_flash_storage_mutex != NULL &&
      xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
  {
    (void)xSemaphoreGive(g_flash_storage_mutex);
  }
}
