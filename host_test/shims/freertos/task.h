#pragma once
#include "FreeRTOS.h"
// Single-threaded host harness: nothing to yield to.
#define taskYIELD() do {} while (0)
