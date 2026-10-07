#pragma once

#include "FreeRTOS.h"

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer);
BaseType_t xSemaphoreTake(SemaphoreHandle_t handle, TickType_t ticks_to_wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t handle);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
