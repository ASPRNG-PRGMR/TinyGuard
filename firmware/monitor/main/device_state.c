/*
 * device_state.c — TinyGuard Monitor
 *
 * Thread-safe store for the latest camera telemetry snapshot.
 * The UDP receiver writes here; future detection/dashboard tasks read here.
 */

#include "device_state.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

static device_state_t  s_state   = {0};
static SemaphoreHandle_t s_mutex = NULL;

void device_state_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    memset(&s_state, 0, sizeof(s_state));
}

void device_state_update(const device_state_t *s)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state = *s;
    xSemaphoreGive(s_mutex);
}

device_state_t device_state_get(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    device_state_t copy = s_state;
    xSemaphoreGive(s_mutex);
    return copy;
}
