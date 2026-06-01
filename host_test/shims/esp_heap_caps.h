#pragma once
#include <stdlib.h>
#define MALLOC_CAP_8BIT 0
#define MALLOC_CAP_SPIRAM 0
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_DEFAULT 0
static inline void* heap_caps_malloc(size_t s,int c){(void)c;return malloc(s);}
static inline void* heap_caps_calloc(size_t n,size_t s,int c){(void)c;return calloc(n,s);}
static inline void* heap_caps_realloc(void*p,size_t s,int c){(void)c;return realloc(p,s);}
static inline size_t heap_caps_get_free_size(int c){(void)c;return (size_t)1<<30;}
static inline size_t heap_caps_get_largest_free_block(int c){(void)c;return (size_t)1<<30;}
