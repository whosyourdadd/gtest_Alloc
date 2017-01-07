#ifndef HUGE_H
#define HUGE_H

#include <stddef.h>

#include "arena.h"

void huge_init(void);
void *huge_alloc(struct thread_cache *cache, size_t size, size_t alignment);
void huge_free(void *ptr);
size_t huge_alloc_size(void *ptr);
void *huge_realloc(struct thread_cache *cache, void *ptr, size_t old_size, size_t new_real_size);

#endif
