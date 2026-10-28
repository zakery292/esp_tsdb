#pragma once
#include "FreeRTOS.h"
typedef void* SemaphoreHandle_t;
static int _m;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void){return &_m;}
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t s,TickType_t t){(void)s;(void)t;return pdTRUE;}
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t s){(void)s;return pdTRUE;}
static inline void vSemaphoreDelete(SemaphoreHandle_t s){(void)s;}
// Recursive-mutex variants used by the vendored global-lock integration.
// Single-threaded host harness, so these are trivial pass-throughs — except
// that a test may define host_lock_event() to observe every outermost take
// (1) and give (0) of a handle lock, e.g. to measure how long it is held.
void host_lock_event(int taken) __attribute__((weak));
__attribute__((weak)) int _rdepth;   // one counter across translation units
static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void){return &_m;}
static inline BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s,TickType_t t){
    (void)s;(void)t; if (_rdepth++ == 0 && host_lock_event) host_lock_event(1); return pdTRUE;}
static inline BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s){
    (void)s; if (--_rdepth == 0 && host_lock_event) host_lock_event(0); return pdTRUE;}
