#include <stdlib.h>
static int refuse_zero_storage;
static void set_refuse_zero_storage(int enabled) { refuse_zero_storage = enabled; }

static void *checked_calloc(size_t count, size_t size) {
	if (refuse_zero_storage && count == 1 && size == 513) return NULL;
	return calloc(count, size);
}
#define calloc(count, size) checked_calloc(count, size)
#ifdef _WIN32
#include <malloc.h>
static void *checked_aligned_malloc(size_t size, size_t alignment) {
	if (refuse_zero_storage && size == 513) return NULL;
	return _aligned_malloc(size, alignment);
}
#define _aligned_malloc(size, alignment) checked_aligned_malloc(size, alignment)
#endif
/* The generated translation unit calls these native adapters. Intercept
 * those calls even when the collector itself is compiled separately. */
void *v_gc_malloc(size_t);
void *v_gc_malloc_atomic(size_t);
static inline void *checked_gc_malloc(size_t size) {
    if (refuse_zero_storage && size == 513) return NULL;
    return v_gc_malloc(size);
}
static inline void *checked_gc_malloc_atomic(size_t size) {
    if (refuse_zero_storage && size == 513) return NULL;
    return v_gc_malloc_atomic(size);
}
#define v_gc_malloc(size) checked_gc_malloc(size)
#define v_gc_malloc_atomic(size) checked_gc_malloc_atomic(size)
