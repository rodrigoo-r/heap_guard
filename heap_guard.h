/*
 * This code is distributed under the terms of the GNU General Public License.
 * For more information, please refer to the LICENSE file in the root directory.
 * -------------------------------------------------
 * Copyright (C) 2025 Rodrigo R.
 * This program comes with ABSOLUTELY NO WARRANTY; for details type show w'.
 * This is free software, and you are welcome to redistribute it
 * under certain conditions; type show c' for details.
*/

#ifndef FLUENT_LIBC_HEAP_GUARD_H
#define FLUENT_LIBC_HEAP_GUARD_H

// ============= FLUENT LIB C =============
// ## Heap Guard API
// ---------------------------------------
// A high-performance, reference-counted memory allocator designed for safe and efficient heap management.
//
// The `heap_guard_t` system provides a robust wrapper around dynamically allocated memory, offering:
// - **Reference counting** for manual shared ownership.
// - **Thread-safety** via optional mutex protection (enabled with `is_concurrent` flag).
// - **Global cleanup** through `atexit()` to prevent memory leaks.
// - **Allocation metadata** to track memory usage and ownership.
// - **Free list management** for efficient memory reuse.
// - **Custom destructors** for user-defined cleanup logic.
//
// ### Key Features
// - Prevents memory leaks by tracking all allocations.
// - Avoids unsafe `free()` calls through reference counting.
// - Supports concurrent access with atomic operations and mutexes.
// - Uses a linked list of heap guards for global cleanup.
// - Integrates with `jemalloc` for optimized memory allocation (if available) or falls back to standard `malloc`.
//
// ### API Overview
// - `heap_alloc`: Allocates memory and returns a heap guard.
// - `raise_guard`: Increments the reference count for shared ownership.
// - `lower_guard`: Decrements the reference count, freeing memory when it reaches zero.
// - `extend_guard`: Resizes the allocated memory block.
// - `drop_guard`: Immediately frees the memory and removes the guard.
// - `heap_destroy`: Cleans up all tracked heap guards (called via `atexit()` or manually).
//
// ### Example Usage
// ```c
// // Allocate 256 bytes with thread-safety enabled
// heap_guard_t *guard = heap_alloc(256, 1);
// if (guard == NULL) {
//     // Handle allocation failure
//     return -1;
// }
//
// // Share ownership by incrementing reference count
// raise_guard(guard);
//
// // Access the memory
// memset(guard->ptr, 0, guard->allocated);
//
// // Release ownership
// lower_guard(&guard); // Memory freed if ref_count reaches 0
// ```
//
// ### Thread-Safety Notes
// - When `is_concurrent` is set, atomic operations and mutexes ensure thread-safe reference counting and guard management.
// - Use `insertion_concurrent` for thread-safe insertion into the global guard list.
// - Non-concurrent mode is faster but not thread-safe.
//
// ### Dependencies
// - `mutex.h`, `atomic.h`, `arena.h`, `vector.h` (from fluent_libc or standard paths in release mode).
// - `jemalloc.h` (if `HAVE_JEMALLOC` is defined via CMake) or `malloc.h` as fallback.
// - `stdlib.h` for standard memory allocation functions.
//
// ### Initial Revision
// - Created: 2025-05-26
// - Author: Rodrigo R.
// ---------------------------------------

// ============= FLUENT LIB C++ =============
#if defined(__cplusplus)
extern "C"
{
#endif

// ============= INCLUDES =============
#ifndef _WIN32
    // Detect jemalloc
#   ifdef HAVE_JEMALLOC // CMake-defined macro
#       include <jemalloc/jemalloc.h>
#else
#       include <malloc.h> // Fallback to standard malloc
#   endif
#else
#       include <malloc.h> // Fallback to standard malloc
#endif

#ifndef FLUENT_LIBC_RELEASE
#   include <mutex.h> // fluent_libc
#   include <atomic.h> // fluent_libc
#   include <arena.h> // fluent_libc
#   include <vector.h> // fluent_libc
#else
#   include <fluent/mutex/mutex.h> // fluent_libc
#   include <fluent/atomic/atomic.h> // fluent_libc
#   include <fluent/arena/arena.h> // fluent_libc
#   include <fluent/vector/vector.h> // fluent_libc
#endif

// ============= MACRO =============
#define DEFINE_HEAP_GUARD(V, NAME, ARENA_SIZE) \
    int __fluent_libc_hg_##NAME##_has_put_atexit_guard = 0; \
                                                            \
    typedef struct heap_guard_##NAME##_t                    \
    {                                                       \
        V *ptr;                                             \
        size_t allocated;                                   \
        size_t ref_count;                                   \
        atomic_size_t concurrent_ref;                       \
        int concurrent;                                     \
        void (*destructor)                                  \
            (const struct heap_guard_##NAME##_t *guard, int is_exit); \
        void *__tracker;                                    \
    } heap_guard_##NAME##_t;                                \
                                                            \
    typedef void (*heap_##NAME##_destructor_t)(const heap_guard_##NAME##_t *guard, int is_exit); \
                                                            \
    typedef struct __fluent_libc_heap_##NAME##_tracker_t    \
    {                                                       \
        heap_guard_##NAME##_t *guard;                       \
        struct __fluent_libc_heap_##NAME##_tracker_t *next; \
        struct __fluent_libc_heap_##NAME##_tracker_t *prev; \
        struct __fluent_libc_heap_##NAME##_tracker_t *tail; \
    } __fluent_libc_heap_##NAME##_tracker_t;                \
                                                            \
    DEFINE_VECTOR(V *, __fluent_libc_hp_fl_##NAME);       \
    DEFINE_VECTOR(heap_guard_##NAME##_t *, __fluent_libc_hph_fl_##NAME); \
    DEFINE_VECTOR(__fluent_libc_heap_##NAME##_tracker_t *, __fluent_libc_hpt_fl_##NAME); \
                                                            \
    __fluent_libc_heap_##NAME##_tracker_t *__fluent_libc_impl_heap_##NAME##_guards = NULL; \
    mutex_t *__fluent_libc_impl_hg_##NAME##_mutex = NULL;   \
                                                            \
    arena_allocator_t *__fluent_libc_hg_##NAME##_arena_allocator = NULL; \
    arena_allocator_t *__fluent_libc_hg_##NAME##_val_arena_allocator = NULL; \
    arena_allocator_t *__fluent_libc_hg_heap_##NAME##_arena_allocator = NULL; \
    vector___fluent_libc_hp_fl_##NAME##_t *__fluent_libc_hg_##NAME##_free_list = NULL; \
    vector___fluent_libc_hph_fl_##NAME##_t *__fluent_libc_hgh_##NAME##_free_list = NULL; \
    vector___fluent_libc_hpt_fl_##NAME##_t *__fluent_libc_hgt_##NAME##_free_list = NULL; \
                                                            \
    static inline void drop_guard_##NAME(                   \
        heap_guard_##NAME##_t **guard_ptr,                  \
        const int is_exit                                   \
    )                                                       \
    {                                                       \
        heap_guard_##NAME##_t *guard = *guard_ptr;          \
                                                            \
        if (guard->destructor != NULL)                      \
        {                                                   \
            guard->destructor(guard, is_exit);              \
        }                                                   \
                                                            \
        if (!is_exit)                                       \
        {                                                   \
            if (guard->ptr != NULL)                         \
            {                                               \
                vec___fluent_libc_hp_fl_##NAME##_push(__fluent_libc_hg_##NAME##_free_list, guard->ptr); \
            }                                               \
                                                            \
            vec___fluent_libc_hph_fl_##NAME##_push(__fluent_libc_hgh_##NAME##_free_list, guard); \
        }                                                   \
                                                            \
        *guard_ptr = NULL;                                  \
    }                                                       \
                                                            \
    static inline void __fluent_libc_hp_##NAME##_destroy()  \
    {                                                       \
        __fluent_libc_heap_##NAME##_tracker_t *current = __fluent_libc_impl_heap_##NAME##_guards; \
                                                            \
        while (current != NULL)                             \
        {                                                   \
            heap_guard_##NAME##_t *guard = current->guard;  \
                                                            \
           if (guard != NULL)                               \
           {                                                \
               drop_guard_##NAME(&guard, 1);                \
           }                                                \
                                                            \
           current = current->next;                         \
        }                                                   \
                                                            \
        if (__fluent_libc_hg_##NAME##_arena_allocator)      \
        {                                                   \
            destroy_arena(__fluent_libc_hg_##NAME##_arena_allocator); \
        }                                                   \
                                                            \
        if (__fluent_libc_hg_heap_##NAME##_arena_allocator) \
        {                                                   \
            destroy_arena(__fluent_libc_hg_heap_##NAME##_arena_allocator); \
        }                                                   \
                                                            \
        if (__fluent_libc_hg_##NAME##_val_arena_allocator)  \
        {                                                   \
            destroy_arena(__fluent_libc_hg_##NAME##_val_arena_allocator); \
        }                                                   \
                                                            \
        if (__fluent_libc_impl_hg_##NAME##_mutex != NULL)   \
        {                                                   \
            mutex_destroy(__fluent_libc_impl_hg_##NAME##_mutex); \
            free(__fluent_libc_impl_hg_##NAME##_mutex);     \
            __fluent_libc_impl_hg_##NAME##_mutex = NULL;    \
        }                                                   \
                                                            \
        if (__fluent_libc_hg_##NAME##_free_list != NULL) \
        {                                                   \
            vec___fluent_libc_hp_fl_##NAME##_destroy(__fluent_libc_hg_##NAME##_free_list, NULL); \
            __fluent_libc_hg_##NAME##_free_list = NULL;     \
        }                                                   \
                                                            \
        if (__fluent_libc_hgh_##NAME##_free_list != NULL)   \
        {                                                   \
            vec___fluent_libc_hph_fl_##NAME##_destroy(__fluent_libc_hgh_##NAME##_free_list, NULL); \
            __fluent_libc_hgh_##NAME##_free_list = NULL;    \
        }                                                   \
                                                            \
        if (__fluent_libc_hgt_##NAME##_free_list != NULL)   \
        {                                                   \
            vec___fluent_libc_hpt_fl_##NAME##_destroy(__fluent_libc_hgt_##NAME##_free_list, NULL); \
            __fluent_libc_hgt_##NAME##_free_list = NULL;    \
        }                                                   \
    }                                                       \
                                                            \
    static __fluent_libc_heap_##NAME##_tracker_t * __fluent_libc_hp_##NAME##_req_tracker() \
    {                                                       \
        if (                                                \
            __fluent_libc_hgt_##NAME##_free_list == NULL || \
            __fluent_libc_hgt_##NAME##_free_list->length == 0 \
        )                                                   \
        {                                                   \
            return (__fluent_libc_heap_##NAME##_tracker_t *)arena_malloc(__fluent_libc_hg_heap_##NAME##_arena_allocator); \
        }                                                   \
                                                            \
        return vec___fluent_libc_hpt_fl_##NAME##_pop(__fluent_libc_hgt_##NAME##_free_list); \
    }                                                       \
                                                            \
    static heap_guard_##NAME##_t * __fluent_libc_hp_##NAME##_req_guard() \
    {                                                       \
        if (                                                \
            __fluent_libc_hgh_##NAME##_free_list == NULL || \
            __fluent_libc_hgh_##NAME##_free_list->length == 0 \
        )                                                   \
        {                                                   \
            return (heap_guard_##NAME##_t *)arena_malloc(__fluent_libc_hg_heap_##NAME##_arena_allocator); \
        }                                                   \
                                                            \
        return vec___fluent_libc_hph_fl_##NAME##_pop(__fluent_libc_hgh_##NAME##_free_list); \
    }                                                       \
                                                            \
    static V *__fluent_libc_hp_##NAME##_req_ptr()           \
    {                                                       \
        if (                                                \
            __fluent_libc_hg_##NAME##_free_list == NULL ||  \
            __fluent_libc_hg_##NAME##_free_list->length == 0 \
        )                                                   \
        {                                                   \
            return (V *)arena_malloc(__fluent_libc_hg_##NAME##_arena_allocator); \
        }                                                   \
                                                            \
        return vec___fluent_libc_hp_fl_##NAME##_pop(__fluent_libc_hg_##NAME##_free_list); \
    }                                                       \
                                                            \
    static inline heap_guard_##NAME##_t *heap_##NAME##_alloc( \
        const int is_concurrent,                            \
        const int insertion_concurrent,                     \
        const heap_##NAME##_destructor_t destructor,        \
        V *default_ptr                                      \
    )                                                       \
    {                                                       \
        if (__fluent_libc_hg_##NAME##_arena_allocator == NULL) \
        {                                                   \
            __fluent_libc_hg_##NAME##_arena_allocator = arena_new(ARENA_SIZE, sizeof(V)); \
        }                                                   \
                                                            \
        if (__fluent_libc_hg_heap_##NAME##_arena_allocator == NULL) \
        {                                                   \
            __fluent_libc_hg_heap_##NAME##_arena_allocator = arena_new(ARENA_SIZE, sizeof(__fluent_libc_heap_##NAME##_tracker_t)); \
        }                                                   \
                                                            \
        if (__fluent_libc_hg_##NAME##_val_arena_allocator == NULL) \
        {                                                   \
            __fluent_libc_hg_##NAME##_val_arena_allocator = arena_new(ARENA_SIZE, sizeof(V)); \
        }                                                   \
                                                            \
        heap_guard_##NAME##_t *guard = __fluent_libc_hp_##NAME##_req_guard(); \
        if (guard == NULL)                                  \
        {                                                   \
            return NULL;                                    \
        }                                                   \
                                                            \
        guard->ptr = default_ptr ? default_ptr : __fluent_libc_hp_##NAME##_req_ptr(); \
        if (guard->ptr == NULL)                             \
        {                                                   \
            return NULL;                                    \
        }                                                   \
                                                            \
        guard->ref_count = 1;                               \
        guard->concurrent = is_concurrent;                  \
        guard->destructor = destructor;                     \
                                                            \
        if (is_concurrent)                                  \
        {                                                   \
            atomic_size_t counter;                          \
            atomic_size_init(&counter, 1);                  \
            guard->concurrent_ref = counter;                \
        }                                                   \
                                                            \
        if (__fluent_libc_hg_##NAME##_has_put_atexit_guard) \
        {                                                   \
            atexit(__fluent_libc_hp_##NAME##_destroy);      \
            __fluent_libc_hg_##NAME##_has_put_atexit_guard = 1; \
        }                                                   \
                                                            \
        if (insertion_concurrent)                           \
        {                                                   \
            if (__fluent_libc_impl_hg_##NAME##_mutex == NULL) \
            {                                               \
                __fluent_libc_impl_hg_##NAME##_mutex = (mutex_t *)malloc(sizeof(mutex_t)); \
                if (__fluent_libc_impl_hg_##NAME##_mutex == NULL) \
                {                                           \
                    return NULL;                            \
                }                                           \
                                                            \
                mutex_init(__fluent_libc_impl_hg_##NAME##_mutex); \
            }                                               \
                                                            \
            mutex_lock(__fluent_libc_impl_hg_##NAME##_mutex); \
        }                                                   \
                                                            \
        if (__fluent_libc_impl_heap_##NAME##_guards == NULL) \
        {                                                   \
            __fluent_libc_impl_heap_##NAME##_guards = __fluent_libc_hp_##NAME##_req_tracker(); \
            if (__fluent_libc_impl_heap_##NAME##_guards == NULL) \
            {                                               \
                if (insertion_concurrent && __fluent_libc_impl_hg_##NAME##_mutex != NULL) \
                {                                           \
                    mutex_unlock(__fluent_libc_impl_hg_##NAME##_mutex); \
                }                                           \
                                                            \
                vec___fluent_libc_hph_fl_##NAME##_push(__fluent_libc_hgh_##NAME##_free_list, guard); \
                vec___fluent_libc_hp_fl_##NAME##_push(__fluent_libc_hg_##NAME##_free_list, guard->ptr); \
                return NULL;                                \
            }                                               \
                                                            \
            guard->__tracker = __fluent_libc_impl_heap_##NAME##_guards; \
            __fluent_libc_impl_heap_##NAME##_guards->guard = guard; \
            __fluent_libc_impl_heap_##NAME##_guards->tail = NULL; \
            __fluent_libc_impl_heap_##NAME##_guards->next = NULL; \
            __fluent_libc_impl_heap_##NAME##_guards->prev = NULL; \
        } else                                              \
        {                                                   \
            __fluent_libc_heap_##NAME##_tracker_t *node = __fluent_libc_hp_##NAME##_req_tracker(); \
                                                            \
            if (node == NULL)                               \
            {                                               \
                if (insertion_concurrent && __fluent_libc_impl_hg_##NAME##_mutex != NULL) \
                {                                           \
                    mutex_unlock(__fluent_libc_impl_hg_##NAME##_mutex); \
                }                                           \
                                                            \
                vec___fluent_libc_hph_fl_##NAME##_push(__fluent_libc_hgh_##NAME##_free_list, guard); \
                vec___fluent_libc_hp_fl_##NAME##_push(__fluent_libc_hg_##NAME##_free_list, guard->ptr); \
                return NULL;                                \
            }                                               \
                                                            \
            guard->__tracker = node;                        \
            node->guard = guard;                            \
            node->next = NULL;                              \
                                                            \
            if (__fluent_libc_impl_heap_##NAME##_guards->tail == NULL) \
            {                                               \
                __fluent_libc_impl_heap_##NAME##_guards->next = node; \
                __fluent_libc_impl_heap_##NAME##_guards->tail = node; \
                node->prev = NULL;                          \
            } else                                          \
            {                                               \
                __fluent_libc_impl_heap_##NAME##_guards->tail->next = node; \
                node->prev = __fluent_libc_impl_heap_##NAME##_guards->tail; \
                __fluent_libc_impl_heap_##NAME##_guards->tail = node; \
            }                                               \
        }                                                   \
                                                            \
        if (insertion_concurrent && __fluent_libc_impl_hg_##NAME##_mutex != NULL) \
        {                                                   \
            mutex_unlock(__fluent_libc_impl_hg_##NAME##_mutex); \
        }                                                   \
                                                            \
        return guard;                                       \
    }                                                       \
                                                            \
    static inline void lower_guard_##NAME(                  \
        heap_guard_##NAME##_t **guard_ptr,                  \
        const int insertion_concurrent                      \
    )                                                       \
    {                                                       \
        if (guard_ptr == NULL || *guard_ptr == NULL)        \
        {                                                   \
            return;                                         \
        }                                                   \
                                                            \
        heap_guard_##NAME##_t *guard = *guard_ptr;          \
                                                            \
        int free_memory = 0;                                \
        if (guard->concurrent)                              \
        {                                                   \
            atomic_size_fetch_sub(&guard->concurrent_ref, 1); \
            free_memory = atomic_size_load(&guard->concurrent_ref) == 0; \
        }                                                   \
        else                                                \
        {                                                   \
            guard->ref_count--;                             \
            free_memory = guard->ref_count == 0;            \
        }                                                   \
                                                            \
        if (free_memory == 1)                               \
        {                                                   \
            if (insertion_concurrent && __fluent_libc_impl_hg_##NAME##_mutex != NULL) \
            {                                               \
                mutex_lock(__fluent_libc_impl_hg_##NAME##_mutex); \
            }                                               \
                                                            \
            __fluent_libc_heap_##NAME##_tracker_t *tracker = (__fluent_libc_heap_##NAME##_tracker_t *)guard->__tracker; \
                                                            \
            if (__fluent_libc_impl_heap_##NAME##_guards->tail == tracker) \
            {                                               \
                __fluent_libc_impl_heap_##NAME##_guards->tail = tracker->prev; \
            }                                               \
                                                            \
            else if (__fluent_libc_impl_heap_##NAME##_guards == tracker) \
            {                                               \
                __fluent_libc_impl_heap_##NAME##_guards = tracker->next; \
            }                                               \
                                                            \
            if (tracker->prev != NULL)                      \
            {                                               \
                tracker->prev->next = tracker->next;        \
                                                            \
                if (tracker->next != NULL)                  \
                {                                           \
                    tracker->next->prev = tracker->prev;    \
                }                                           \
            }                                               \
                                                            \
            vector___fluent_libc_hpt_fl_##NAME##_push(__fluent_libc_hgt_##NAME##_free_list, tracker); \
                                                            \
            drop_guard_##NAME(guard_ptr, 0);                \
                                                            \
            if (insertion_concurrent && __fluent_libc_impl_hg_##NAME##_mutex != NULL) \
            {                                               \
                mutex_unlock(__fluent_libc_impl_hg_##NAME##_mutex); \
            }                                               \
        }                                                   \
    }
// ============= FLUENT LIB C++ =============
#if defined(__cplusplus)
}
#endif

#endif //FLUENT_LIBC_HEAP_GUARD_H