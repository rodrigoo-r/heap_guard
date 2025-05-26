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
#include <mutex.h> // fluent_libc
#include <hash_map.h> // fluent_libc

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
typedef struct
{
    void *ptr;        // Pointer to the allocated memory
    size_t allocated; // Total bytes allocated
    size_t ref_count; // Reference count for the heap guard
    mutex_t *mutex; // Optional mutex for thread safety
    int concurrent;   // Flag to indicate if the allocation is concurrent
    size_t id; // Unique identifier for the allocation
} heap_guard_t;

/**
 * @brief Global pointer to the hashmap tracking all heap_guard_t allocations.
 *
 * This hashmap is used for centralized management and cleanup of all
 * heap-guarded allocations within the program.
 */
hashmap_t *heap_guards = NULL;

/**
 * @brief Global pointer to a hashmap tracking available keys for heap guards.
 *
 * This hashmap may be used to manage or recycle unique identifiers (keys)
 * for heap_guard_t allocations, supporting efficient reuse and management
 * of allocation IDs within the heap guard system.
 */
hashmap_t *available_keys = NULL;

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
 */
inline void drop_guard(heap_guard_t **guard_ptr)
{
    // Get the pointer to the guard
    heap_guard_t *guard = *guard_ptr;

    if (guard->ptr != NULL)
    {
        free(guard->ptr); // Free the allocated memory block
    }

    if (guard->concurrent)
    {
        mutex_destroy(guard->mutex); // Destroy the mutex if it exists
        free(guard->mutex); // Free the mutex memory
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
inline void heap_destroy()
{
    // Iterate through the map
    hashmap_iter_t iter = hashmap_iter_begin(heap_guards);
    hash_entry_t* entry;

    while ((entry = hashmap_iter_next(&iter)) != NULL) {
        // Get the guard from the entry
        heap_guard_t *guard = (heap_guard_t *)entry->value; // Cast for C++ compatibility

        // Free the guard and its resources
        if (guard != NULL)
        {
            free(entry->key); // Free the key memory
            drop_guard(&guard); // Free the guard and its resources
        }
    }

    // Destroy the hashmap itself
    hashmap_free(heap_guards);

    // Destroy the available keys hashmap if it exists
    if (available_keys != NULL)
    {
        hashmap_free(available_keys);
        available_keys = NULL; // Reset the pointer to NULL
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
 * @return Pointer to the initialized heap_guard_t structure, or NULL on failure.
 */
inline heap_guard_t *heap_alloc(const size_t size, const int is_concurrent)
{
    // Allocate memory for the heap_guard_t structure
    heap_guard_t *guard = (heap_guard_t *)malloc(sizeof(heap_guard_t)); // Cast for C++ compatibility
    if (guard == NULL)
    {
        return NULL; // Allocation failed
    }

    // Initialize the guard structure
    guard->ptr = malloc(size); // Allocate the actual memory block
    if (guard->ptr == NULL)
    {
        free(guard); // Clean up if allocation fails
        return NULL; // Allocation failed
    }

    // Find a unique ID for the guard
    size_t available_id = 0;
    if (available_keys != NULL && available_keys->count > 0)
    {
        // Get an iterator to the map
        hashmap_iter_t iter = hashmap_iter_begin(available_keys);
        const hash_entry_t *entry = hashmap_iter_next(&iter);
        if (entry != NULL)
        {
            // Use the first available key as the ID
            available_id = *(size_t *)entry->key; // Cast for C++ compatibility
            hashmap_remove(available_keys, entry->key); // Remove it from available keys
        } else {
            // No available keys, use the current count as the ID
            available_id = heap_guards ? heap_guards->count : 0;
        }
    }

    // Initialize metadata
    guard->allocated = size;
    guard->ref_count = 1; // Start with a reference count of 1
    guard->concurrent = is_concurrent; // Set the concurrency flag
    guard->id = available_id; // Unique ID based on current count in the hashmap

    // Initialize the mutex if concurrency is enabled
    if (is_concurrent)
    {
        // Allocate a mutex for thread safety
        mutex_t *mutex = (mutex_t *)malloc(sizeof(mutex_t)); // Cast for C++ compatibility
        if (mutex == NULL)
        {
            free(guard->ptr); // Clean up if mutex allocation fails
            free(guard);
            return NULL; // Allocation failed
        }

        mutex_init(mutex); // Initialize the mutex
        guard->mutex = mutex;
    } else {
        guard->mutex = NULL; // No mutex needed
    }

    // Check if we have to set automatic cleanup
    if (__fluent_libc_has_put_atexit_guard == 0)
    {
        // Register the cleanup function to be called at program exit
        atexit(heap_destroy);
        __fluent_libc_has_put_atexit_guard = 1; // Set the flag to indicate cleanup is registered
    }

    // Check if we have to initialize the linked list
    if (heap_guards == NULL)
    {
        heap_guards = hashmap_new(
            FLUENT_LIBC_HEAP_MAP_CAPACITY,
            FLUENT_LIBC_HEAP_MAP_GROWTH_F,
            NULL,
            (hash_function_t)hash_int, // Cast for C++ compatibility
            0 // Don't free keys on deletion
        );
    } else {
        hashmap_insert(
            heap_guards,
            &guard->id, // Use the unique ID as the key
            guard // Store the guard as the value
        );
    }

    // Initialize the available keys hashmap if it doesn't exist
    if (available_keys == NULL)
    {
        available_keys = hashmap_new(
            FLUENT_LIBC_HEAP_MAP_CAPACITY,
            FLUENT_LIBC_HEAP_MAP_GROWTH_F,
            NULL,
            (hash_function_t)hash_int, // Cast for C++ compatibility
            1 // Free keys on deletion
        );
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
inline void raise_guard(heap_guard_t *guard)
{
    if (guard == NULL)
    {
        return; // Nothing to raise
    }

    // Check if we have a mutex
    if (guard->concurrent)
    {
        mutex_lock(guard->mutex);
    }

    // Increment the reference count
    guard->ref_count++;

    // Unlock the mutex if it was locked
    if (guard->concurrent)
    {
        mutex_unlock(guard->mutex);
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
 */
inline void lower_guard(heap_guard_t **guard_ptr)
{
    // Check if the guard is freed
    if (guard_ptr == NULL || *guard_ptr == NULL)
    {
        return; // Nothing to lower
    }

    // Get the pointer to the guard
    heap_guard_t *guard = *guard_ptr;

    // Check if we have a mutex
    if (guard->concurrent)
    {
        mutex_lock(guard->mutex);
    }

    // Increment the reference count
    guard->ref_count--;

    // Check if we have to drop the guard
    if (guard->ref_count == 0)
    {
        // Remove the guard from the global hashmap
        hashmap_remove(heap_guards, &guard->id);

        // Unlock the mutex right before freeing the guard
        if (guard->concurrent)
        {
            mutex_unlock(guard->mutex);
        }

        // Clone the ID for further reuse
        size_t *id = (size_t *)malloc(sizeof(size_t));
        memcpy(id, &guard->id, sizeof(size_t));

        // Mark the ID as available in the available keys hashmap
        hashmap_insert(available_keys, id, NULL); // Insert the ID with NULL value

        // Drop the guard and free its resources
        drop_guard(guard_ptr);
        return; // Guard has been freed, exit the function
    }

    // Unlock the mutex if it was locked
    if (guard->concurrent)
    {
        mutex_unlock(guard->mutex);
    }
}

/**
 * @brief Extends the memory block managed by a heap_guard_t allocation.
 *
 * This function attempts to reallocate the memory block pointed to by the given
 * heap_guard_t structure, increasing its size by the specified amount. If the
 * allocation is concurrent, the associated mutex is locked before performing the
 * reallocation and unlocked afterward to ensure thread safety. On success, the
 * pointer and allocated size in the guard are updated.
 *
 * @param guard Pointer to the heap_guard_t structure whose memory block is to be extended.
 *              Must not be NULL and must point to a valid allocation.
 * @param size  The number of bytes to add to the current allocation.
 * @return      1 if the reallocation was successful, 0 otherwise.
 */
inline int extend_guard(heap_guard_t *guard, const size_t size)
{
    if (guard == NULL || guard->ptr == NULL)
    {
        return 0; // Nothing to extend
    }

    // Check if we have a mutex
    if (guard->concurrent)
    {
        mutex_lock(guard->mutex);
    }

    // Reallocate the memory block to the new size
    void *new_ptr = realloc(guard->ptr, guard->allocated + size);

    // Handle failure
    if (new_ptr == NULL)
    {
        // Release the mutex if it was locked
        if (guard->concurrent)
        {
            mutex_unlock(guard->mutex);
        }

        // Return false
        return 0; // Reallocation failed
    }

    guard->ptr = new_ptr; // Update the pointer if realloc was successful
    guard->allocated += size; // Update the allocated size

    // Unlock the mutex if it was locked
    if (guard->concurrent)
    {
        mutex_unlock(guard->mutex);
    }

    return 1; // Reallocation successful
}

// ============= FLUENT LIB C++ =============
#if defined(__cplusplus)
}
#endif

#endif //FLUENT_LIBC_HEAP_GUARD_H