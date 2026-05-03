#include "control_queue.h"

#include "cmsis_os.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#include <string.h>

typedef struct
{
  control_proto_command_t command;
  relay_domain_result_t *result;
  TaskHandle_t waiting_task;
} control_queue_request_t;

#define CONTROL_QUEUE_LENGTH             8U
#define CONTROL_QUEUE_WAIT_TIMEOUT_MS    100U

static control_queue_stats_t g_control_queue_stats;
static QueueHandle_t g_control_queue_handle = NULL;
static osThreadId g_control_queue_task_handle = NULL;

static void control_queue_worker_task(void const *argument)
{
  control_queue_request_t request;

  (void)argument;

  for (;;)
  {
    if (xQueueReceive(g_control_queue_handle, &request, portMAX_DELAY) == pdPASS)
    {
      relay_domain_execute(&request.command, request.result);
      g_control_queue_stats.executed++;

      if (request.waiting_task != NULL)
      {
        xTaskNotifyGive(request.waiting_task);
      }
    }
  }
}

void control_queue_init(void)
{
  static uint8_t initialized = 0U;

  if (initialized != 0U)
  {
    return;
  }

  memset(&g_control_queue_stats, 0, sizeof(g_control_queue_stats));
  g_control_queue_handle = xQueueCreate(CONTROL_QUEUE_LENGTH, sizeof(control_queue_request_t));

  if (g_control_queue_handle != NULL)
  {
    osThreadDef(ControlQueueWorker, control_queue_worker_task, osPriorityAboveNormal, 0, 256);
    g_control_queue_task_handle = osThreadCreate(osThread(ControlQueueWorker), NULL);
  }

  initialized = 1U;
}

HAL_StatusTypeDef control_queue_submit(const control_proto_command_t *command, relay_domain_result_t *result)
{
  control_queue_request_t request;

  if ((command == NULL) || (result == NULL))
  {
    g_control_queue_stats.dropped++;
    return HAL_ERROR;
  }

  control_queue_init();
  if ((g_control_queue_handle == NULL) || (g_control_queue_task_handle == NULL))
  {
    g_control_queue_stats.dropped++;
    return HAL_ERROR;
  }

  request.command = *command;
  request.result = result;
  request.waiting_task = xTaskGetCurrentTaskHandle();

  (void)ulTaskNotifyTake(pdTRUE, 0U);

  if (xQueueSend(g_control_queue_handle, &request, 0U) != pdPASS)
  {
    g_control_queue_stats.dropped++;
    return HAL_BUSY;
  }

  g_control_queue_stats.submitted++;

  if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CONTROL_QUEUE_WAIT_TIMEOUT_MS)) == 0U)
  {
    g_control_queue_stats.dropped++;
    return HAL_TIMEOUT;
  }

  return HAL_OK;
}

void control_queue_poll(void)
{
  control_queue_init();
}

void control_queue_get_stats(control_queue_stats_t *stats)
{
  if (stats != NULL)
  {
    *stats = g_control_queue_stats;
  }
}
