#pragma once
#include "task.h"
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t function,
    const char *name, uint32_t stack, void *argument, UBaseType_t priority,
    TaskHandle_t *handle, BaseType_t core, UBaseType_t caps);
