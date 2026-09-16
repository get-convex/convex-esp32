#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM   (1u << 0)
#define MALLOC_CAP_INTERNAL (1u << 1)
static inline void  *heap_caps_malloc(size_t n, unsigned) { return malloc(n); }
static inline void  *heap_caps_realloc(void *p, size_t n, unsigned) { return realloc(p, n); }
static inline void   heap_caps_free(void *p) { free(p); }
static inline size_t heap_caps_get_free_size(unsigned) { return 4u << 20; }
static inline size_t heap_caps_get_largest_free_block(unsigned) { return 1u << 20; }
