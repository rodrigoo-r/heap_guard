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
// heap_guard_t API
// ----------------------------------------
// Smart heap allocation wrapper with optional thread-safety.
//
// heap_guard_t wraps a memory block with:
// - allocation metadata
// - reference counting (manual shared ownership)
// - optional mutex protection
// - global cleanup with atexit()
//
// This system helps prevent:
// - memory leaks
// - unsafe free()
// - race conditions (via `concurrent` flag and mutex)
//
// Linked list of heap guards is used for global cleanup tracking.
//
// Function Signatures:
// ----------------------------------------
// heap_guard_t *heap_alloc(size_t size, int is_concurrent);
// void raise_guard(heap_guard_t *guard);
// void lower_guard(heap_guard_t **guard_ptr);
// int  extend_guard(heap_guard_t *guard, size_t size);
// void drop_guard(heap_guard_t **guard_ptr);
// void heap_destroy(void);
//
// Example:
// ----------------------------------------
// heap_guard_t *guard = heap_alloc(256, 1);
// raise_guard(guard);
// lower_guard(&guard); // Automatically freed when ref count = 0
//
// ----------------------------------------
// Initial revision: 2025-05-26
// ----------------------------------------
// Depends on: mutex.h, optional.h, stdlib.h, jemalloc (optional)
// ----------------------------------------

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
#else
#   include <fluent/mutex/mutex.h> // fluent_libc
#   include <fluent/atomic/atomic.h> // fluent_libc
#   include <fluent/arena/arena.h> // fluent_libc
#endif

// ============= MACROS =============
#ifndef FLUENT_LIBC_HEAP_POOL_CAPACITY // Define if not user-defined
#   define FLUENT_LIBC_HEAP_POOL_CAPACITY 50 // Default capacity for the hashmap
#endif

// Flag to check if automatic cleanup is enabled
int __fluent_libc_has_put_atexit_guard = 0;

/**
 * @brief Structure representing a guarded heap allocation.
 *
 * This structure is used to manage a block of dynamically allocated memory,
 * providing metadata for memory management, reference counting, and optional
 * thread safety.
 *
 * Members:
 *   ptr        Pointer to the allocated memory block.
 *   allocated  Total number of bytes allocated.
 *   ref_count  Reference count for the allocation, used for shared ownership.
 *   mutex      Optional mutex for thread safety (may be empty if not used).
 *   concurrent Flag indicating if the allocation is concurrent (1 for yes, 0 for no).
 *   id         Unique identifier for the allocation.
 */
typedef struct heap_guard_t
{
    void *ptr;        // Pointer to the allocated memory
    size_t allocated; // Total bytes allocated
    size_t ref_count; // Reference count for the heap guard
    atomic_size_t concurrent_ref; // Atomic reference count for concurrent access
    int concurrent;   // Flag to indicate if the allocation is concurrent
    int key_concurrent; // Flag to indicate if the key is concurrent
    void (*destructor) // Destructor function pointer for cleanup
        (const struct heap_guard_t *guard, int is_exit);
    void *__tracker; // Pointer to the internal tracker for this guard
                     // (Warning: should not be used directly by users)
} heap_guard_t;

/**
 * @brief Function pointer type for a heap guard destructor.
 *
 * This type defines a function that takes a pointer to a `heap_guard_t` (representing
 * a heap-guarded allocation) and an integer flag `is_exit` indicating whether
 * the destructor is being called during program exit (non-zero) or during normal operation (zero).
 * The function is responsible for cleaning up resources associated with the heap guard.
 *
 * @param guard Pointer to the `heap_guard_t` structure to be destroyed.
 * @param is_exit Non-zero if called during program exit, zero otherwise.
 */
typedef void (*heap_destructor_t)(const heap_guard_t *guard, int is_exit);

/**
 * @brief Doubly-linked list node for tracking heap_guard_t allocations.
 *
 * This structure is used to maintain a linked list of all active heap_guard_t allocations,
 * enabling centralized management and cleanup. Each node contains a pointer to a heap_guard_t,
 * as well as pointers to the next and previous nodes in the list.
 */
typedef struct __fluent_libc_heap_tracker_t
{
    heap_guard_t *guard;              // Pointer to the heap guard structure
    struct __fluent_libc_heap_tracker_t *next; // Pointer to the next tracker in the linked list
    struct __fluent_libc_heap_tracker_t *prev; // Pointer to the previous tracker in the linked list
    struct __fluent_libc_heap_tracker_t *tail; // Pointer to the tail of the linked list for easy access
} __fluent_libc_heap_tracker_t;

/**
 * @brief Global pointer to the hashmap tracking all heap_guard_t allocations.
 *
 * This hashmap is used for centralized management and cleanup of all
 * heap-guarded allocations within the program.
 */
__fluent_libc_heap_tracker_t *__fluent_libc_impl_heap_guards = NULL;
mutex_t *__fluent_libc_impl_hg_mutex = NULL; // Mutex for thread safety of available keys

// Global arena allocator for allocating heap guards
arena_allocator_t *__fluent_libc_hg_arena_allocator = NULL;
arena_allocator_t *__fluent_libc_hg_heap_arena_allocator = NULL;

/**
 * @brief Frees a heap_guard_t allocation and its associated resources.
 *
 * This function releases the memory block managed by the given heap_guard_t,
 * destroys and frees the associated mutex if concurrency is enabled, and
 * finally frees the heap_guard_t structure itself. The pointer to the guard
 * is set to NULL to prevent dangling references.
 *
 * @param guard_ptr Double pointer to the heap_guard_t structure to be freed.
 *                  After the function returns, *guard_ptr will be set to NULL.
 * @param is_exit   Non-zero if called during program exit, zero otherwise.
 */
static inline void drop_guard(heap_guard_t **guard_ptr, const int is_exit)
{
    // Get the pointer to the guard
    heap_guard_t *guard = *guard_ptr;

    // Check if we have a destructor function
    if (guard->destructor != NULL)
    {
        // Call the destructor function with the guard and is_exit flag
        guard->destructor(guard, is_exit);
    }

    if (guard->ptr != NULL)
    {
        free(guard->ptr); // Free the allocated memory block
    }

    *guard_ptr = NULL; // Set the pointer to NULL to avoid dangling pointers
}

/**
 * @brief Frees all heap-guarded allocations and associated resources.
 *
 * This function iterates through the global linked list of heap guards,
 * freeing each allocated memory block, destroying and freeing any associated
 * mutex, and finally freeing the list node itself. After calling this function,
 * all memory managed by the heap guard system will be released.
 */
static inline void heap_destroy()
{
    // Iterate through the list
    const __fluent_libc_heap_tracker_t *current = __fluent_libc_impl_heap_guards;

    while (current != NULL) {
        // Get the guard from the entry
        heap_guard_t *guard = current->guard;

        // Free the guard and its resources
        if (guard != NULL)
        {
            drop_guard(&guard, 1); // Free the guard and its resources
        }

        // Move to the next node in the linked list
        current = current->next;
    }

    // Destroy the arena allocators
    if (__fluent_libc_hg_arena_allocator)
    {
        destroy_arena(__fluent_libc_hg_arena_allocator);
    }

    if (__fluent_libc_hg_heap_arena_allocator)
    {
        destroy_arena(__fluent_libc_hg_heap_arena_allocator);
    }

    // Destroy the mutex if it exists
    if (__fluent_libc_impl_hg_mutex != NULL)
    {
        mutex_destroy(__fluent_libc_impl_hg_mutex); // Destroy the mutex
        free(__fluent_libc_impl_hg_mutex); // Free the mutex memory
        __fluent_libc_impl_hg_mutex = NULL; // Reset the pointer to NULL
    }
}

/**
 * @brief Allocates a new heap_guard_t structure and manages it in the global linked list.
 *
 * This function allocates a block of memory of the specified size and wraps it in a
 * heap_guard_t structure, which includes metadata for memory management, reference counting,
 * and optional thread safety via a mutex. The allocation is tracked in a global doubly-linked
 * list for centralized management and cleanup. If concurrency is enabled, a mutex is created
 * and associated with the allocation. The function also ensures that a cleanup handler is
 * registered to free all allocations at program exit.
 *
 * @param size The number of bytes to allocate for the memory block.
 * @param is_concurrent Non-zero to enable thread safety (allocates a mutex), zero otherwise.
 * @param insertion_concurrent Non-zero to enable concurrent insertion into the hashmap, zero otherwise.
 * @param destructor Function pointer for a custom destructor to be called when the guard is freed.
 * @param default_ptr Pointer to a default value to be used instead of allocating memory.
 *                    If NULL, a new memory block is allocated.
 * @return Pointer to the initialized heap_guard_t structure, or NULL on failure.
 */
static inline heap_guard_t *heap_alloc(
    const size_t size,
    const int is_concurrent,
    const int insertion_concurrent,
    const heap_destructor_t destructor,
    void *default_ptr
)
{
    // Check if the arena allocator is NULL
    if (__fluent_libc_hg_arena_allocator == NULL)
    {
        __fluent_libc_hg_arena_allocator = arena_new(FLUENT_LIBC_HEAP_POOL_CAPACITY, sizeof(heap_guard_t));
    }

    if (__fluent_libc_hg_heap_arena_allocator == NULL)
    {
        __fluent_libc_hg_heap_arena_allocator = arena_new(FLUENT_LIBC_HEAP_POOL_CAPACITY, sizeof(__fluent_libc_heap_tracker_t));
    }

    // Allocate memory for the heap_guard_t structure
    heap_guard_t *guard = (heap_guard_t *)arena_malloc(__fluent_libc_hg_arena_allocator); // Cast for C++ compatibility
    if (guard == NULL)
    {
        return NULL; // Allocation failed
    }

    // Initialize the guard structure
    guard->ptr = default_ptr ? default_ptr : malloc(size); // Allocate the actual memory block
    if (guard->ptr == NULL)
    {
        free(guard); // Clean up if allocation fails
        return NULL; // Allocation failed
    }

    // Initialize metadata
    guard->allocated = size;
    guard->ref_count = 1; // Start with a reference count of 1
    guard->concurrent = is_concurrent; // Set the concurrency flag
    guard->destructor = destructor; // Set the destructor function pointer

    // Initialize the mutex if concurrency is enabled
    if (is_concurrent)
    {
        atomic_size_t counter;
        atomic_size_init(&counter, 1);
        guard->concurrent_ref = counter; // Initialize the atomic reference count
    }

    // Check if we have to set automatic cleanup
    if (__fluent_libc_has_put_atexit_guard == 0)
    {
        // Register the cleanup function to be called at program exit
        atexit(heap_destroy);
        __fluent_libc_has_put_atexit_guard = 1; // Set the flag to indicate cleanup is registered
    }

    // Lock the mutex if insertion is concurrent
    if (insertion_concurrent)
    {
        if (__fluent_libc_impl_hg_mutex == NULL)
        {
            __fluent_libc_impl_hg_mutex = (mutex_t *)malloc(sizeof(mutex_t)); // Cast for C++ compatibility
            if (__fluent_libc_impl_hg_mutex == NULL)
            {
                free(guard->ptr); // Clean up if mutex allocation fails
                free(guard);
                return NULL; // Allocation failed
            }

            mutex_init(__fluent_libc_impl_hg_mutex); // Initialize the mutex
        }

        mutex_lock(__fluent_libc_impl_hg_mutex); // Lock the mutex for thread safety
    }

    // Check if we have to initialize the linked list
    if (__fluent_libc_impl_heap_guards == NULL)
    {
        // Allocate the tracker
        __fluent_libc_impl_heap_guards = (__fluent_libc_heap_tracker_t *)arena_malloc(__fluent_libc_hg_heap_arena_allocator);
        // Handle allocation failure
        if (__fluent_libc_impl_heap_guards == NULL)
        {
            if (insertion_concurrent && __fluent_libc_impl_hg_mutex != NULL)
            {
                mutex_unlock(__fluent_libc_impl_hg_mutex); // Unlock the mutex before returning
            }

            free(guard->ptr); // Clean up if tracker allocation fails
            free(guard);
            return NULL; // Allocation failed
        }

        // Initialize the tracker
        guard->__tracker = __fluent_libc_impl_heap_guards; // Set the tracker pointer
        __fluent_libc_impl_heap_guards->guard = guard; // Set the guard pointer
        __fluent_libc_impl_heap_guards->tail = NULL; // Initialize tail to NULL
        __fluent_libc_impl_heap_guards->next = NULL; // Initialize next pointer to NULL
        __fluent_libc_impl_heap_guards->prev = NULL; // Initialize previous pointer to NULL
    } else
    {
        // Create a new node
        __fluent_libc_heap_tracker_t * node =(__fluent_libc_heap_tracker_t *)arena_malloc(__fluent_libc_hg_heap_arena_allocator);

        // Handle allocation failure
        if (node == NULL)
        {
            if (insertion_concurrent && __fluent_libc_impl_hg_mutex != NULL)
            {
                mutex_unlock(__fluent_libc_impl_hg_mutex); // Unlock the mutex before returning
            }

            free(guard->ptr); // Clean up if node allocation fails
            free(guard);
            return NULL; // Allocation failed
        }

        // Initialize the new node
        guard->__tracker = node; // Set the tracker pointer to the new node
        node->guard = guard; // Set the guard pointer
        node->next = NULL; // Initialize next pointer to NULL

        // Check if the linked list is empty
        if (__fluent_libc_impl_heap_guards->tail == NULL)
        {
            // If empty, set both head and tail to the new node
            __fluent_libc_impl_heap_guards->next = node;
            __fluent_libc_impl_heap_guards->tail = node;
            node->prev = NULL; // Set previous pointer to NULL
        }
        else
        {
            // If not empty, append the new node to the end of the list
            __fluent_libc_impl_heap_guards->tail->next = node;
            node->prev = __fluent_libc_impl_heap_guards->tail; // Set previous pointer to current tail
            __fluent_libc_impl_heap_guards->tail = node; // Update tail to the new node
        }
    }

    // Unlock the mutex if it was locked
    if (insertion_concurrent && __fluent_libc_impl_hg_mutex != NULL)
    {
        mutex_unlock(__fluent_libc_impl_hg_mutex); // Unlock the mutex after insertion
    }

    // Return the initialized heap guard
    return guard;
}

/**
 * @brief Increments the reference count of a heap_guard_t allocation.
 *
 * If the allocation is concurrent, this function locks the associated mutex
 * before incrementing the reference count and unlocks it afterward to ensure
 * thread safety.
 *
 * @param guard Pointer to the heap_guard_t structure whose reference count is to be incremented.
 *              If NULL, the function does nothing.
 */
static inline void raise_guard(heap_guard_t *guard)
{
    if (guard == NULL)
    {
        return; // Nothing to raise
    }

    // Unlock the mutex if it was locked
    if (guard->concurrent)
    {
        atomic_size_fetch_add(&guard->concurrent_ref, 1); // Increment the atomic reference count
    }
    else
    {
        // Increment the reference count
        guard->ref_count++;
    }
}

/**
 * @brief Decrements the reference count of a heap_guard_t allocation and frees it if needed.
 *
 * This function safely decrements the reference count of the heap_guard_t structure pointed to by guard_ptr.
 * If the allocation is concurrent, the associated mutex is locked before modifying the reference count and
 * unlocked afterward to ensure thread safety. If the reference count reaches zero, the guard and its resources
 * are freed, and the pointer is set to NULL to prevent dangling references.
 *
 * @param guard_ptr Double pointer to the heap_guard_t structure whose reference count is to be decremented.
 *                  If NULL or if *guard_ptr is NULL, the function does nothing.
 *
 * @param  insertion_concurrent Non-zero to enable concurrent insertion into the hashmap, zero otherwise.
 */
static inline void lower_guard(heap_guard_t **guard_ptr, const int insertion_concurrent)
{
    // Check if the guard is freed
    if (guard_ptr == NULL || *guard_ptr == NULL)
    {
        return; // Nothing to lower
    }

    // Get the pointer to the guard
    heap_guard_t *guard = *guard_ptr;

    // Check if we have a concurrent allocation
    int free_memory = 0;
    if (guard->concurrent)
    {
        atomic_size_fetch_sub(&guard->concurrent_ref, 1); // Decrement the atomic reference count
        free_memory = atomic_size_load(&guard->concurrent_ref) == 0; // Check if we need to free memory
    }
    else
    {
        // Increment the reference count
        guard->ref_count--;
        free_memory = guard->ref_count == 0; // Check if we need to free memory
    }

    // Check if we have to drop the guard
    if (free_memory == 1)
    {
        // Lock the mutex if insertion is concurrent
        if (insertion_concurrent && __fluent_libc_impl_hg_mutex != NULL)
        {
            mutex_lock(__fluent_libc_impl_hg_mutex); // Lock the mutex for thread safety
        }

        // Get the tracker from the guard
        __fluent_libc_heap_tracker_t *tracker = (__fluent_libc_heap_tracker_t *)guard->__tracker;

        // Handle case: the tracker is the tail node
        if (tracker->tail == tracker)
        {
            tracker->tail = tracker->prev; // Update the tail to the previous node
        }

        // Relocate the pointers
        if (tracker->prev != NULL)
        {
            tracker->prev->next = tracker->next; // Link previous node to next

            // Relocate the prev node of the next node
            if (tracker->next != NULL)
            {
                tracker->next->prev = tracker->prev; // Link next node to previous
            }
        }

        // Drop the guard and free its resources
        drop_guard(guard_ptr, 0);

        // Unlock the mutex
        if (insertion_concurrent && __fluent_libc_impl_hg_mutex != NULL)
        {
            mutex_unlock(__fluent_libc_impl_hg_mutex); // Unlock the mutex after insertion
        }
    }
}

/**
 * @brief Resizes the memory block managed by a heap_guard_t allocation.
 *
 * This function attempts to reallocate the memory block pointed to by the given
 * heap_guard_t structure to the specified new size. On success, the pointer and allocated size
 * in the guard are updated.
 *
 * @param guard Pointer to the heap_guard_t structure whose memory block is to be resized.
 *              Must not be NULL and must point to a valid allocation.
 * @param size  The new size in bytes for the memory block.
 * @return      1 if the reallocation was successful, 0 otherwise.
 */
static inline int resize_guard(heap_guard_t *guard, const size_t size)
{
    if (guard == NULL || guard->ptr == NULL)
    {
        return 0; // Nothing to extend
    }

    // Reallocate the memory block to the new size
    void *new_ptr = realloc(guard->ptr, size);

    // Handle failure
    if (new_ptr == NULL)
    {
        // Return false
        return 0; // Reallocation failed
    }

    guard->ptr = new_ptr; // Update the pointer if realloc was successful
    guard->allocated = size; // Update the allocated size

    return 1; // Reallocation successful
}

/**
 * @brief Extends the memory block managed by a heap_guard_t allocation.
 *
 * This function attempts to reallocate the memory block pointed to by the given
 * heap_guard_t structure, increasing its size by the specified amount. If the
 * allocation is concurrent, On success, the pointer and allocated size in the guard are updated.
 *
 * @param guard Pointer to the heap_guard_t structure whose memory block is to be extended.
 *              Must not be NULL and must point to a valid allocation.
 * @param size  The number of bytes to add to the current allocation.
 * @return      1 if the reallocation was successful, 0 otherwise.
 */
static inline int extend_guard(heap_guard_t *guard, const size_t size)
{
    return resize_guard(guard, guard->allocated + size);
}


// ============= FLUENT LIB C++ =============
#if defined(__cplusplus)
}
#endif

#endif //FLUENT_LIBC_HEAP_GUARD_H