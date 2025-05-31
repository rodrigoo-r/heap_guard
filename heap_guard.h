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
#   include <hashmap.h> // fluent_libc
#   include <atomic.h> // fluent_libc
#else
#   include <fluent/mutex/mutex.h> // fluent_libc
#   include <fluent/hashmap/hashmap.h> // fluent_libc
#   include <fluent/atomic/atomic.h> // fluent_libc
#endif

// ============= MACROS =============
#ifndef FLUENT_LIBC_HEAP_MAP_CAPACITY // Define if not user-defined
#   define FLUENT_LIBC_HEAP_MAP_CAPACITY 1024 // Default capacity for the hashmap
#endif

#ifndef FLUENT_LIBC_HEAP_MAP_GROWTH_F // Define if not user-defined
#   define FLUENT_LIBC_HEAP_MAP_GROWTH_F 1.5 // Growth factor for the hashmap
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
    size_t id; // Unique identifier for the allocation
    void (*destructor) // Destructor function pointer for cleanup
        (const struct heap_guard_t *guard, int is_exit);
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

#ifndef FLUENT_LIBC_HEAP_GUARD_MAP_DEFINED // Define if not user-defined
    DEFINE_HASHMAP(size_t, heap_guard_t *, _heap); // Define the hashmap for heap guards
    DEFINE_HASHMAP(size_t, void *, _heap_k); // Define the hashmap for the free keys
#   define FLUENT_LIBC_HEAP_GUARD_MAP_DEFINED 1 // Flag to indicate heap guard map is defined
#endif

// ============= CMP FUNCTION =============
static inline int size_t_cmp(const size_t a, const size_t b)
{
    return a == b;
}

/**
 * @brief Global pointer to the hashmap tracking all heap_guard_t allocations.
 *
 * This hashmap is used for centralized management and cleanup of all
 * heap-guarded allocations within the program.
 */
hashmap__heap_t *__fluent_libc_impl_heap_guards = NULL;

/**
 * @brief Global pointer to a hashmap tracking available keys for heap guards.
 *
 * This hashmap may be used to manage or recycle unique identifiers (keys)
 * for heap_guard_t allocations, supporting efficient reuse and management
 * of allocation IDs within the heap guard system.
 */
hashmap__heap_k_t *__fluent_libc_impl_available_keys = NULL;
mutex_t *__fluent_libc_impl_available_keys_mutex = NULL; // Mutex for thread safety of available keys

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

    free(guard); // Free the heap_guard_t structure
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
    // Iterate through the map
    hashmap__heap_iter_t iter = hashmap__heap_iter_begin(__fluent_libc_impl_heap_guards);
    hash__heap_entry_t* entry;

    while ((entry = hashmap__heap_iter_next(&iter)) != NULL) {
        // Get the guard from the entry
        heap_guard_t *guard = entry->value;

        // Free the guard and its resources
        if (guard != NULL)
        {
            drop_guard(&guard, 1); // Free the guard and its resources
        }
    }

    // Destroy the hashmap itself
    hashmap__heap_free(__fluent_libc_impl_heap_guards);

    // Destroy the available keys hashmap if it exists
    if (__fluent_libc_impl_available_keys != NULL)
    {
        hashmap__heap_k_free(__fluent_libc_impl_available_keys);
        __fluent_libc_impl_available_keys = NULL; // Reset the pointer to NULL
    }

    // Destroy the available keys mutex if it exists
    if (__fluent_libc_impl_available_keys_mutex != NULL)
    {
        mutex_destroy(__fluent_libc_impl_available_keys_mutex); // Destroy the mutex
        free(__fluent_libc_impl_available_keys_mutex); // Free the mutex memory
        __fluent_libc_impl_available_keys_mutex = NULL; // Reset the pointer to NULL
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
    // Allocate memory for the heap_guard_t structure
    heap_guard_t *guard = (heap_guard_t *)malloc(sizeof(heap_guard_t)); // Cast for C++ compatibility
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

    // Find a unique ID for the guard
    size_t available_id = 0;
    if (__fluent_libc_impl_available_keys != NULL && __fluent_libc_impl_available_keys->count > 0)
    {
        // Get an iterator to the map
        hashmap__heap_k_iter_t iter = hashmap__heap_k_iter_begin(__fluent_libc_impl_available_keys);
        const hash__heap_k_entry_t *entry = hashmap__heap_k_iter_next(&iter);
        if (entry != NULL)
        {
            // Use the first available key as the ID
            available_id = *(size_t *)entry->key; // Cast for C++ compatibility
            hashmap__heap_k_remove(__fluent_libc_impl_available_keys, entry->key); // Remove it from available keys
        } else {
            // No available keys, use the count as the ID
            available_id = __fluent_libc_impl_heap_guards ? __fluent_libc_impl_heap_guards->count : 0;
        }
    }

    // Initialize metadata
    guard->allocated = size;
    guard->ref_count = 1; // Start with a reference count of 1
    guard->concurrent = is_concurrent; // Set the concurrency flag
    guard->id = available_id; // Unique ID based on current count in the hashmap
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
        if (__fluent_libc_impl_available_keys_mutex == NULL)
        {
            __fluent_libc_impl_available_keys_mutex = (mutex_t *)malloc(sizeof(mutex_t)); // Cast for C++ compatibility
            if (__fluent_libc_impl_available_keys_mutex == NULL)
            {
                free(guard->ptr); // Clean up if mutex allocation fails
                free(guard);
                return NULL; // Allocation failed
            }

            mutex_init(__fluent_libc_impl_available_keys_mutex); // Initialize the mutex
        }

        mutex_lock(__fluent_libc_impl_available_keys_mutex); // Lock the mutex for thread safety
    }

    // Check if we have to initialize the linked list
    if (__fluent_libc_impl_heap_guards == NULL)
    {
        __fluent_libc_impl_heap_guards = hashmap__heap_new(
            FLUENT_LIBC_HEAP_MAP_CAPACITY,
            FLUENT_LIBC_HEAP_MAP_GROWTH_F,
            NULL,
            (hash__heap_function_t)hash_int, // Cast for C++ compatibility
            (hash__heap_cmp_t)size_t_cmp
        );
    } else {
        hashmap__heap_insert(
            __fluent_libc_impl_heap_guards,
            guard->id, // Use the unique ID as the key
            guard // Store the guard as the value
        );
    }

    // Initialize the available keys hashmap if it doesn't exist
    if (__fluent_libc_impl_available_keys == NULL)
    {
        __fluent_libc_impl_available_keys = hashmap__heap_k_new(
            FLUENT_LIBC_HEAP_MAP_CAPACITY,
            FLUENT_LIBC_HEAP_MAP_GROWTH_F,
            NULL,
            (hash__heap_k_function_t)hash_int, // Cast for C++ compatibility
            (hash__heap_k_cmp_t)size_t_cmp
        );
    }

    // Unlock the mutex if it was locked
    if (insertion_concurrent && __fluent_libc_impl_available_keys_mutex != NULL)
    {
        mutex_unlock(__fluent_libc_impl_available_keys_mutex); // Unlock the mutex after insertion
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
        if (insertion_concurrent && __fluent_libc_impl_available_keys_mutex != NULL)
        {
            mutex_lock(__fluent_libc_impl_available_keys_mutex); // Lock the mutex for thread safety
        }

        // Remove the guard from the global hashmap
        hashmap__heap_remove(__fluent_libc_impl_heap_guards, guard->id);

        // Mark the ID as available in the available keys hashmap
        hashmap__heap_k_insert(__fluent_libc_impl_available_keys, guard->id, NULL); // Insert the ID with NULL value

        // Drop the guard and free its resources
        drop_guard(guard_ptr, 0);

        // Unlock the mutex
        if (insertion_concurrent && __fluent_libc_impl_available_keys_mutex != NULL)
        {
            mutex_unlock(__fluent_libc_impl_available_keys_mutex); // Unlock the mutex after insertion
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