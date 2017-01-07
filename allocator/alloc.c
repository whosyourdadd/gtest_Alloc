#define RB_COMPACT

#include <errno.h>
#include <pthread.h>
#include <malloc.h>
#include <sched.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <stdio.h> //
#include "arena.h"
#include "bump.h"
#include "chunk.h"
#include "huge.h"
#include "memory.h"
#include "mutex.h"
#include "purge.h"
#include "util.h"
#include "rb.h"

#ifndef thread_local
#define thread_local _Thread_local
#endif

#define LARGE_ALIGN (sizeof(struct large))
#define LARGE_MASK (sizeof(struct large) - 1)
#define MIN_ALIGN 16
#define SLAB_SIZE (64 * 1024)
#define CACHE_SIZE (16 * 1024)
#define MAX_SMALL 512
#define LARGE_CHUNK_HEADER ((sizeof(struct chunk) + LARGE_MASK) & ~LARGE_MASK)
#define MAX_LARGE (CHUNK_SIZE - (LARGE_CHUNK_HEADER + sizeof(struct large)))

#if INTPTR_MAX == INT32_MAX
#define INITIAL_VA ((size_t)256 * 1024 * 1024)
#else
#define INITIAL_VA ((size_t)1024 * 1024 * 1024 * 1024)
#endif

static_assert(INITIAL_VA % CHUNK_SIZE == 0, "INITIAL_VA not a multiple of CHUNK_SIZE");

static int large_addr_comp(struct large *a, struct large *b) {
    uintptr_t a_addr = (uintptr_t)a;
    uintptr_t b_addr = (uintptr_t)b;
    return (a_addr > b_addr) - (a_addr < b_addr);
}

static int large_size_addr_comp(struct large *a, struct large *b) {
    size_t a_size = a->size;
    size_t b_size = b->size;

    int ret = (a_size > b_size) - (a_size < b_size);
    if (ret) {
        return ret;
    }

    return large_addr_comp(a, b);
}

rb_gen(, large_tree_size_addr_, large_tree, struct large, link_size_addr, large_size_addr_comp)

static bool init_failed = false;
static atomic_bool initialized = ATOMIC_VAR_INIT(false);
static mutex init_mutex = MUTEX_INITIALIZER;

static struct arena *arenas;
static int n_arenas = 0;

static void *reserved_start;
static void *reserved_end;
static size_t arena_initial_va_log2;

static pthread_key_t tcache_key;

__attribute__((tls_model("initial-exec")))
static thread_local struct thread_cache tcache = {{NULL}, {0}, -1, true};

struct arena *get_huge_arena(void *ptr) {
    if (ptr >= reserved_start && ptr < reserved_end) {
        size_t diff = (char *)ptr - (char *)reserved_start;
        return arenas + (diff >> arena_initial_va_log2);
    }
    return NULL;
}

static inline struct slab *to_slab(void *ptr) {
    return ALIGNMENT_ADDR2BASE(ptr, SLAB_SIZE);
}

static void slab_deallocate(struct arena *arena, struct slab *slab, struct slot *ptr, size_t bin);

static void tcache_destroy(void *key) {
    struct thread_cache *cache = key;
    for (int a = 0; a < n_arenas; a++) {
        struct arena *arena = &arenas[a];
        bool locked = false;
        for (size_t bin = 0; bin < N_CLASS; bin++) {
            struct slot **last_next = &cache->bin[bin];
            struct slot *slot = cache->bin[bin];

            while (slot) {
                struct slot *next = slot->next;
                struct chunk *chunk = CHUNK_ADDR2BASE(slot);
                assert(chunk->small);
                if (chunk->arena == a) {
                    if (!locked) {
                        mutex_lock(&arena->mutex);
                        locked = true;
                    }
                    slab_deallocate(arena, to_slab(slot), slot, bin);
                    *last_next = next;
                } else {
                    last_next = &slot->next;
                }
                slot = next;
            }
        }
        if (locked) {
            mutex_unlock(&arena->mutex);
        }
    }
    cache->dead = true;
}

static void pick_arena(struct thread_cache *cache) {
    cache->arena_index = sched_getcpu();
    if (unlikely(cache->arena_index == -1 || cache->arena_index > n_arenas)) {
        cache->arena_index = 0;
    }
}

static void thread_init(struct thread_cache *cache) {
    pick_arena(cache);
    if (likely(!pthread_setspecific(tcache_key, cache))) {
        cache->dead = false;
    }
}

static bool malloc_init_slow(struct thread_cache *cache) {
    if (likely(atomic_load_explicit(&initialized, memory_order_consume))) {
        thread_init(cache);
        return false;
    }

    mutex_lock(&init_mutex);

    if (atomic_load_explicit(&initialized, memory_order_consume)) {
        mutex_unlock(&init_mutex);
        thread_init(cache);
        return false;
    }

    if (unlikely(init_failed)) {
        return true;
    }

    n_arenas = get_nprocs();
    arenas = bump_alloc(sizeof(struct arena) * n_arenas, alignof(struct arena));
    if (!arenas) {
        init_failed = true;
        mutex_unlock(&init_mutex);
        return true;
    }

    if (pthread_key_create(&tcache_key, tcache_destroy)) {
        init_failed = true;
        mutex_unlock(&init_mutex);
        return true;
    }

    memory_init();
    chunk_init();
    huge_init();
    purge_init();

    struct rlimit limit;
    void *reserved = NULL;
    arena_initial_va_log2 = size_log2(INITIAL_VA / n_arenas);
    size_t arena_initial_va = (size_t)1 << arena_initial_va_log2;
    size_t total_initial_va = arena_initial_va * n_arenas;
    if (arena_initial_va >= CHUNK_SIZE
        && !getrlimit(RLIMIT_AS, &limit) && limit.rlim_cur == RLIM_INFINITY) {
        reserved = memory_map_aligned(NULL, total_initial_va, CHUNK_SIZE, false);
        if (reserved) {
            reserved_start = reserved;
            reserved_end = (char *)reserved + total_initial_va;
        }
    }

    for (int i = 0; i < n_arenas; i++) {
        struct arena *arena = &arenas[i];
        if (mutex_init(&arena->mutex)) {
            init_failed = true;
            mutex_unlock(&init_mutex);
            return true;
        }
        for (size_t bin = 0; bin < N_CLASS; bin++) {
#ifndef NDEBUG
            arena->partial_slab[bin].prev = (struct slab *)0xdeadbeef;
#endif
            arena->partial_slab[bin].next = &arena->partial_slab[bin];
        }
        large_tree_size_addr_new(&arena->large_size_addr);
        extent_tree_ad_new(&arena->huge);

        chunk_recycler_init(&arena->chunks);
        if (reserved) {
            chunk_free(&arena->chunks, reserved, arena_initial_va);
            arena->chunks_start = reserved;
            reserved = arena->chunks_end = (char *)reserved + arena_initial_va;
        }
    }

    atomic_store_explicit(&initialized, true, memory_order_release);

    mutex_unlock(&init_mutex);
    thread_init(cache);
    return false;
}

static bool malloc_init(struct thread_cache *cache) {
    if (likely(cache->arena_index != -1)) {
        return false;
    }
    return malloc_init_slow(cache);
}

inline struct arena *get_arena(struct thread_cache *cache) {
    if (unlikely(mutex_trylock(&arenas[cache->arena_index].mutex))) {
        pick_arena(cache);
        mutex_lock(&arenas[cache->arena_index].mutex);
    }
    return &arenas[cache->arena_index];
}

static void *arena_chunk_alloc(struct arena *arena) {
    if (arena->free_chunk) {
        struct chunk *chunk = arena->free_chunk;
        arena->free_chunk = NULL;
        return chunk;
    }
    void *chunk = chunk_recycle(&arena->chunks, NULL, CHUNK_SIZE, CHUNK_SIZE);
    if (chunk) {
        if (unlikely(memory_commit(chunk, CHUNK_SIZE))) {
            chunk_free(&arena->chunks, chunk, CHUNK_SIZE);
            return NULL;
        }
    } else {
        chunk = chunk_alloc(NULL, CHUNK_SIZE, CHUNK_SIZE);
        if (unlikely(!chunk)) {
            return NULL;
        }
    }
    ((struct chunk *)chunk)->arena = arena - arenas;
    return chunk;
}

static void arena_chunk_free(struct arena *arena, void *chunk) {
    if (arena->free_chunk) {
        if (purge_ratio >= 0) {
            memory_decommit(arena->free_chunk, CHUNK_SIZE);
        }
        if (chunk >= arena->chunks_start && chunk < arena->chunks_end) {
            chunk_free(&arena->chunks, arena->free_chunk, CHUNK_SIZE);
        } else {
            chunk_free(NULL, arena->free_chunk, CHUNK_SIZE);
        }
    }
    arena->free_chunk = chunk;
}

static void *slab_first_alloc(struct arena *arena, struct slab *slab, size_t size, size_t bin) {
    slab->prev = &arena->partial_slab[bin];
    slab->size = size;
    slab->count = 1;
    void *first = (void *)ALIGNMENT_CEILING((uintptr_t)slab->data, MIN_ALIGN);
    slab->next_slot = (struct slot *)((char *)first + size);
    slab->next_slot->next = NULL;
    slab->end = (struct slot *)((char *)slab->next_slot + size);
    return first;
}

static void *slab_allocate(struct arena *arena, size_t size, size_t bin) {
    // check for the sentinel node terminating the list
    if (!arena->partial_slab[bin].next->next_slot) {
        if (arena->free_slab) {
            struct slab *slab = arena->free_slab;
            arena->free_slab = arena->free_slab->next;

            slab->next = arena->partial_slab[bin].next;
            arena->partial_slab[bin].next = slab;

            return slab_first_alloc(arena, slab, size, bin);
        }

        struct chunk *chunk = arena_chunk_alloc(arena);
        if (unlikely(!chunk)) {
            return NULL;
        }
        chunk->small = true;

        struct slab *slab = (struct slab *)ALIGNMENT_CEILING((uintptr_t)chunk->data, SLAB_SIZE);
        slab->next = arena->partial_slab[bin].next;
        arena->partial_slab[bin].next = slab;

        void *chunk_end = (char *)chunk + CHUNK_SIZE;
        while ((uintptr_t)slab + SLAB_SIZE < (uintptr_t)chunk_end) {
            slab = (struct slab *)((char *)slab + SLAB_SIZE);
            slab->next = arena->free_slab;
            arena->free_slab = slab;
        }

        return slab_first_alloc(arena, arena->partial_slab[bin].next, size, bin);
    }

    struct slab *slab = arena->partial_slab[bin].next;
    struct slot *slot = slab->next_slot;
    slab->next_slot = slot->next;
    slab->count++;
    if (!slab->next_slot) {
        uintptr_t new_end = (uintptr_t)slab->end + size;
        if (new_end > (uintptr_t)slab + SLAB_SIZE) {
            struct slab *next = slab->next;
            next->prev = &arena->partial_slab[bin];
            arena->partial_slab[bin].next = next;
        } else {
            slab->next_slot = slab->end;
            slab->next_slot->next = NULL;
            slab->end = (struct slot *)new_end;
        }
    }

    return slot;
}

static size_t size2bin(size_t size) {
    return (size >> 4) - 1;
}

static void slab_deallocate(struct arena *arena, struct slab *slab, struct slot *slot, size_t bin) {
    slot->next = slab->next_slot;
    slab->next_slot = slot;
    slab->count--;

    if (!slot->next) {
        struct slab *next = arena->partial_slab[bin].next;
        slab->next = next;
        slab->prev = &arena->partial_slab[bin];
        next->prev = slab;
        arena->partial_slab[bin].next = slab;
    } else if (!slab->count) {
        slab->prev->next = slab->next;
        slab->next->prev = slab->prev;

        slab->next = arena->free_slab;
        arena->free_slab = slab;
    }
}

static inline void *allocate_small(struct thread_cache *cache, size_t size) {
    size_t bin = size2bin(size);

    if (unlikely(cache->dead)) {
        if (cache->arena_index == -1 && unlikely(malloc_init(cache))) {
            return NULL;
        }
        if (unlikely(cache->dead)) {
            struct arena *arena = get_arena(cache);
            void *ptr = slab_allocate(arena, size, bin);
            mutex_unlock(&arena->mutex);
            return ptr;
        }
    }

    struct slot *slot = cache->bin[bin];
    if (likely(slot)) {
        cache->bin[bin] = slot->next;
        cache->bin_size[bin] -= size;
        return slot;
    }

    struct arena *arena = get_arena(cache);
    void *ptr = slab_allocate(arena, size, bin);

    while (cache->bin_size[bin] + size < CACHE_SIZE / 2) {
        struct slot *slot = slab_allocate(arena, size, bin);
        if (!slot) {
            mutex_unlock(&arena->mutex);
            return ptr;
        }
        slot->next = cache->bin[bin];
        cache->bin[bin] = slot;
        cache->bin_size[bin] += size;
    }

    mutex_unlock(&arena->mutex);
    return ptr;
}

static const struct large *const used_sentinel = (void *)0x1;

static bool is_used(const struct large *large) {
    return large->link_size_addr.rbn_left == used_sentinel;
}

static void mark_used(struct large *large) {
    large->link_size_addr.rbn_left = (struct large *)used_sentinel;
}

static struct large *to_head(void *ptr) {
    return (struct large *)((char *)ptr - sizeof(struct large));
}

static void update_next_span(void *ptr, size_t size) {
    struct large *next = (struct large *)((char *)ptr + size);
    if (next <= to_head((void *)CHUNK_CEILING((uintptr_t)next))) {
        next->prev = ptr;
    }
}

static void large_free(struct arena *arena, void *span, size_t size) {
    struct large *self = span;
    self->size = size;

    struct large *next = (void *)((char *)span + size);

    // Try to coalesce forward.
    if (next <= to_head((void *)CHUNK_CEILING((uintptr_t)next)) && !is_used(next)) {
        // Coalesce span with the following address range.
        large_tree_size_addr_remove(&arena->large_size_addr, next);
        self->size += next->size;
    }

    // Try to coalesce backward.
    struct large *prev = ((struct large *)span)->prev;
    if (prev && !is_used(prev)) {
        // Coalesce span with the previous address range.
        assert((char *)prev + prev->size == (char *)self);
        large_tree_size_addr_remove(&arena->large_size_addr, prev);
        size_t new_size = self->size + prev->size;
        self = prev;
        self->size = new_size;
    }

    if (self->size == CHUNK_SIZE - LARGE_CHUNK_HEADER) {
        arena_chunk_free(arena, (struct chunk *)((char *)self - LARGE_CHUNK_HEADER));
    } else {
        large_tree_size_addr_insert(&arena->large_size_addr, self);
        update_next_span(self, self->size);
    }
}

static struct large *large_recycle(struct arena *arena, size_t size, size_t alignment) {
    size_t full_size = size + sizeof(struct large);
    size_t alloc_size = full_size + alignment - LARGE_ALIGN;
    assert(alloc_size >= full_size);
    struct large key;
    key.size = alloc_size;
    struct large *span = large_tree_size_addr_nsearch(&arena->large_size_addr, &key);
    if (!span) {
        return NULL;
    }

    void *data = (void *)ALIGNMENT_CEILING((uintptr_t)span + sizeof(struct large), alignment);
    struct large *head = to_head(data);

    size_t leadsize = (char *)head - (char *)span;
    assert(span->size >= leadsize + full_size);
    size_t trailsize = span->size - leadsize - full_size;

    // Remove free span from the tree.
    large_tree_size_addr_remove(&arena->large_size_addr, span);
    if (leadsize) {
        // Insert the leading space as a smaller span.
        span->size = leadsize;
        large_tree_size_addr_insert(&arena->large_size_addr, span);
        update_next_span(span, span->size);
    }
    if (trailsize) {
        // Insert the trailing space as a smaller span.
        struct large *trail = (struct large *)((char *)head + full_size);
        trail->size = trailsize;
        large_tree_size_addr_insert(&arena->large_size_addr, trail);
        update_next_span(trail, trail->size);
    }

    update_next_span(head, full_size);
    head->size = size;
    mark_used(head);
    return head;
}

static void *allocate_large(struct thread_cache *cache, size_t size, size_t alignment) {
    assert(alignment >= LARGE_ALIGN);

    struct arena *arena = get_arena(cache);

    struct large *head = large_recycle(arena, size, alignment);
    if (head) {
        mutex_unlock(&arena->mutex);
        return head->data;
    }

    struct chunk *chunk = arena_chunk_alloc(arena);
    if (unlikely(!chunk)) {
        mutex_unlock(&arena->mutex);
        return NULL;
    }
    chunk->small = false;

    void *base = (char *)chunk + LARGE_CHUNK_HEADER;
    void *data = (void *)ALIGNMENT_CEILING((uintptr_t)base + sizeof(struct large), alignment);
    head = to_head(data);
    head->size = size;
    head->prev = NULL;

    update_next_span(head, size + sizeof(struct large));
    mark_used(head);

    if (head != base) {
        assert(alignment > MIN_ALIGN);
        size_t lead = (char *)head - (char *)base;
        head = (struct large *)((char *)base);
        head->size = lead;
        head->prev = NULL;
        large_free(arena, base, lead);
    }

    void *end = (char *)head->data + size;
    void *chunk_end = (char *)chunk + CHUNK_SIZE;
    if (end != chunk_end) {
        large_free(arena, end, (char *)chunk_end - (char *)end);
    }

    mutex_unlock(&arena->mutex);

    return head->data;
}

static bool large_expand_recycle(struct arena *arena, void *new_addr, size_t size) {
    assert(new_addr);
    assert(ALIGNMENT_ADDR2BASE(new_addr, MIN_ALIGN) == new_addr);

    if (new_addr > (void *)to_head((void *)CHUNK_CEILING((uintptr_t)new_addr))) {
        return true;
    }

    struct large *next = new_addr;
    if (is_used(next) || next->size < size) {
        return true;
    }

    // Remove node from the tree.
    large_tree_size_addr_remove(&arena->large_size_addr, next);

    size_t trailsize = next->size - size;
    if (trailsize) {
        // Insert the trailing space as a smaller span.
        struct large *trail = (struct large *)((char *)next + size);
        trail->size = trailsize;
        large_tree_size_addr_insert(&arena->large_size_addr, trail);
        update_next_span(trail, trail->size);
    }

    return false;
}

static bool large_realloc_no_move(void *ptr, size_t old_size, size_t new_size) {
    struct chunk *chunk = CHUNK_ADDR2BASE(ptr);
    assert(!chunk->small);
    struct arena *arena = &arenas[chunk->arena];
    struct large *head = to_head(ptr);

    if (old_size < new_size) {
        void *expand_addr = (char *)ptr + old_size;
        size_t expand_size = new_size - old_size;

        mutex_lock(&arena->mutex);
        if (large_expand_recycle(arena, expand_addr, expand_size)) {
            mutex_unlock(&arena->mutex);
            return true;
        }
        head->size = new_size;
        update_next_span(head, new_size + sizeof(struct large));
        mutex_unlock(&arena->mutex);
    } else if (new_size < old_size) {
        void *excess_addr = (char *)ptr + new_size;
        size_t excess_size = old_size - new_size;

        mutex_lock(&arena->mutex);
        update_next_span(head, new_size + sizeof(struct large));
        head->size = new_size;
        large_free(arena, excess_addr, excess_size);
        mutex_unlock(&arena->mutex);
    }

    return false;
}

static inline void *allocate(struct thread_cache *cache, size_t size) {
    if (size <= MAX_SMALL) {
        size_t non_zero_size = size | (!size);
        size_t real_size = (non_zero_size + 15) & ~15;
        return allocate_small(cache, real_size);
    }

    if (unlikely(malloc_init(cache))) {
        return NULL;
    }

    if (size <= MAX_LARGE) {
        size_t real_size = (size + LARGE_MASK) & ~LARGE_MASK;
        return allocate_large(cache, real_size, LARGE_ALIGN);
    }
    return huge_alloc(cache, size, CHUNK_SIZE);
}

static inline void deallocate_small(struct thread_cache *cache, void *ptr) {
    struct slot *slot = ptr;
    struct slab *slab = to_slab(slot);
    size_t size = slab->size;
    size_t bin = size2bin(size);

    if (unlikely(cache->dead)) {
        if (cache->arena_index == -1) {
            thread_init(cache);
        }
        if (unlikely(cache->dead)) {
            struct chunk *chunk = CHUNK_ADDR2BASE(slot);
            struct arena *arena = &arenas[chunk->arena];
            mutex_lock(&arena->mutex);
            slab_deallocate(arena, slab, slot, bin);
            mutex_unlock(&arena->mutex);
            return;
        }
    }

    slot->next = cache->bin[bin];
    cache->bin[bin] = slot;
    cache->bin_size[bin] += size;

    if (unlikely(cache->bin_size[bin] > CACHE_SIZE)) {
        cache->bin_size[bin] = size;
        while (cache->bin_size[bin] < CACHE_SIZE / 2) {
            slot = slot->next;
            assert(slot);
            cache->bin_size[bin] += size;
        }

        struct slot *flush = slot->next;
        slot->next = NULL;

        do {
            struct slot *slot = flush;
            flush = NULL;

            int arena_index = ((struct chunk *)CHUNK_ADDR2BASE(slot))->arena;
            struct arena *arena = &arenas[arena_index];
            mutex_lock(&arena->mutex);
            do {
                struct slot *next = slot->next;

                struct chunk *chunk = CHUNK_ADDR2BASE(slot);
                assert(chunk->small);
                if (chunk->arena == arena_index) {
                    slab_deallocate(arena, to_slab(slot), slot, bin);
                    slot = slot->next;
                } else {
                    slot->next = flush;
                    flush = slot;
                }

                slot = next;
            } while (slot);
            mutex_unlock(&arena->mutex);
        } while (flush);
    }
}

static inline void deallocate(struct thread_cache *cache, void *ptr) {
    // malloc_init has been called if the pointer is non-NULL
    assert(!ptr || atomic_load(&initialized));

    struct chunk *chunk = CHUNK_ADDR2BASE(ptr);
    if (ptr == chunk) {
        if (!ptr) {
            return;
        }
        huge_free(ptr);
        return;
    }
    if (chunk->small) {
        deallocate_small(cache, ptr);
    } else {
        struct arena *arena = &arenas[chunk->arena];
        mutex_lock(&arena->mutex);
        struct large *head = to_head(ptr);
        large_free(arena, head, head->size + sizeof(struct large));
        mutex_unlock(&arena->mutex);
    }
}

static size_t alloc_size(void *ptr) {
    // malloc_init has been called if the pointer is non-NULL
    assert(!ptr || atomic_load(&initialized));

    struct chunk *chunk = CHUNK_ADDR2BASE(ptr);
    if (ptr == chunk) {
        if (!ptr) {
            return 0;
        }
        return huge_alloc_size(ptr);
    }
    if (chunk->small) {
        return to_slab(ptr)->size;
    }
    return to_head(ptr)->size;
}

static int alloc_aligned_result(void **memptr, void *ptr) {
    if (unlikely(!ptr)) {
        return ENOMEM;
    }
    *memptr = ptr;
    return 0;
}

static int alloc_aligned(void **memptr, size_t alignment, size_t size, size_t min_alignment) {
    assert(min_alignment != 0);

    if (unlikely((alignment - 1) & alignment || alignment < min_alignment)) {
        return EINVAL;
    }

    struct thread_cache *cache = &tcache;

    if (alignment <= MIN_ALIGN) {
        return alloc_aligned_result(memptr, allocate(cache, size));
    }

    size_t non_zero_size = size | (!size);
    size_t large_size = (non_zero_size + LARGE_MASK) & ~LARGE_MASK;
    size_t large_alignment = (alignment + LARGE_MASK) & ~LARGE_MASK;
    size_t worst_large_size = large_size + large_alignment - LARGE_ALIGN;
    if (unlikely(worst_large_size < size)) {
        return ENOMEM;
    }

    if (unlikely(malloc_init(cache))) {
        return ENOMEM;
    }

    if (worst_large_size <= MAX_LARGE) {
        return alloc_aligned_result(memptr, allocate_large(cache, large_size, large_alignment));
    }
    return alloc_aligned_result(memptr, huge_alloc(cache, size, CHUNK_CEILING(alignment)));
}

static void *alloc_aligned_simple(size_t alignment, size_t size) {
    void *ptr;
    int ret = alloc_aligned(&ptr, alignment, size, 1);
    if (unlikely(ret)) {
        errno = ret;
        return NULL;
    }
    return ptr;
}

EXPORT void *malloc(size_t size) {
    void *ptr = allocate(&tcache, size);
    if (unlikely(!ptr)) {
        errno = ENOMEM;
        return NULL;
    }
    return ptr;
}

EXPORT void *calloc(size_t nmemb, size_t size) {
    size_t total;
    if (unlikely(size_mul_overflow(nmemb, size, &total))) {
        errno = ENOMEM;
        return NULL;
    }
    void *new_ptr = allocate(&tcache, total);
    if (unlikely(!new_ptr)) {
        errno = ENOMEM;
        return NULL;
    }
    memset(new_ptr, 0, total);
    return new_ptr;
}

EXPORT void *realloc(void *ptr, size_t size) {
    if (!ptr) {
        return malloc(size);
    }

    // malloc_init has been called
    assert(atomic_load(&initialized));

    struct thread_cache *cache = &tcache;

    // Marked obsolete in DR400
    if (unlikely(!size)) {
        deallocate(cache, ptr);
        return NULL;
    }

    size_t old_size = alloc_size(ptr);

    size_t real_size = (size + 15) & ~15;
    if (old_size == real_size) {
        return ptr;
    }

    if (old_size <= MAX_LARGE && real_size <= MAX_LARGE &&
        old_size > MAX_SMALL && real_size > MAX_SMALL) {
        size_t real_size = (size + LARGE_MASK) & ~LARGE_MASK;
        if (!large_realloc_no_move(ptr, old_size, real_size)) {
            return ptr;
        }
    }
    if (old_size > MAX_LARGE && size > MAX_LARGE) {

        return huge_realloc(cache, ptr, old_size, CHUNK_CEILING(size));
    }

    void *new_ptr = allocate(cache, size);
    if (unlikely(!new_ptr)) {
        errno = ENOMEM;
        return NULL;
    }
    size_t copy_size = size < old_size ? size : old_size;
    memcpy(new_ptr, ptr, copy_size);
    deallocate(cache, ptr);
    return new_ptr;
}

EXPORT void free(void *ptr) {
    deallocate(&tcache, ptr);
}

EXPORT void cfree(void *ptr) __attribute__((alias("free")));

EXPORT int posix_memalign(void **memptr, size_t alignment, size_t size) {
    return alloc_aligned(memptr, alignment, size, sizeof(void *));
}

EXPORT void *aligned_alloc(size_t alignment, size_t size) {
    // Comply with the semantics specified in DR460
    if (unlikely(size % alignment)) {
        errno = EINVAL;
        return NULL;
    }
    return alloc_aligned_simple(alignment, size);
}

EXPORT void *memalign(size_t alignment, size_t size) {
    return alloc_aligned_simple(alignment, size);
}

EXPORT void *valloc(size_t size) {
    return alloc_aligned_simple(PAGE_SIZE, size);
}

EXPORT void *pvalloc(size_t size) {
    size_t rounded = PAGE_CEILING(size);
    if (unlikely(!rounded)) {
        errno = ENOMEM;
        return NULL;
    }
    return alloc_aligned_simple(PAGE_SIZE, rounded);
}

EXPORT size_t malloc_usable_size(void *ptr) {
    return alloc_size(ptr);
}

COLD EXPORT int malloc_trim(UNUSED size_t pad) {
    return 0;
}

COLD EXPORT void malloc_stats(void) {}

COLD EXPORT struct mallinfo mallinfo(void) {
    return (struct mallinfo){0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
}

COLD EXPORT int mallopt(UNUSED int param, UNUSED int value) {
    return 1;
}

COLD EXPORT int malloc_info(UNUSED int options, UNUSED FILE *fp) {
    return ENOSYS;
}

COLD EXPORT void *malloc_get_state(void) {
    return NULL;
}

COLD EXPORT int malloc_set_state(UNUSED void *state) {
    return -2;
}
