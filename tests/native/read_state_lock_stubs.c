#include "freertos/semphr.h"

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer)
{
    return buffer;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks_to_wait)
{
    (void)handle;
    (void)ticks_to_wait;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t handle)
{
    (void)handle;
    return pdTRUE;
}
