#pragma once
#include <stdint.h>
typedef int BaseType_t; typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t; typedef uint8_t StackType_t;
#define portMAX_DELAY 0xffffffffUL
#define pdTRUE 1
#define pdFALSE 0
static inline UBaseType_t uxTaskGetStackHighWaterMark(void*t){(void)t;return 4096;}
#define pdMS_TO_TICKS(x) (x)
