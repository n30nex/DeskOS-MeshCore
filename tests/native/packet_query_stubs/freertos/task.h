#pragma once
#include "freertos/FreeRTOS.h"
typedef void *TaskHandle_t;
typedef unsigned UBaseType_t;
typedef void (*TaskFunction_t)(void *);
#define pdPASS 1
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
void xTaskNotifyGive(TaskHandle_t task);
