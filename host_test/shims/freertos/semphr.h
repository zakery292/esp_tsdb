#pragma once
#include "FreeRTOS.h"
typedef void* SemaphoreHandle_t;
static int _m;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void){return &_m;}
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s,TickType_t t){(void)s;(void)t;return pdTRUE;}
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s){(void)s;return pdTRUE;}
static inline void vSemaphoreDelete(SemaphoreHandle_t s){(void)s;}
// Recursive-mutex variants used by the vendored global-lock integration.
// Single-threaded host harness, so these are trivial pass-throughs.
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void){return &_m;}
static inline BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s,TickType_t t){(void)s;(void)t;return pdTRUE;}
static inline BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s){(void)s;return pdTRUE;}
