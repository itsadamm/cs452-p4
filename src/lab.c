#include "lab.h"
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>  // For debugging purposes (e.g., perror)

/* Detailed Notes:

1. Included <sys/mman.h> for mmap() and munmap() system calls required for memory management.
2. Included <errno.h> to set and check error numbers (e.g., ENOMEM).
3. Included <string.h> for memcpy in buddy_realloc.
4. Included <stdio.h> for debugging functions like perror.

Implementations of functions as per lab.h:

*/

/**
 * Converts bytes to its equivalent K value defined as bytes <= 2^K
 * @param bytes The bytes needed
 * @return K The number of bytes expressed as 2^K
 */
size_t btok(size_t bytes) {
    /* Notes:
     - Since we cannot use log2, we will calculate K by finding the position of the highest set bit.
     - We need to find the smallest K such that 2^K >= bytes.
     - We'll use bit manipulation to achieve this.
    */

    if (bytes == 0) {
        return 0;
    }

    /* Adjust bytes to include the size of the header (struct avail) */
    bytes += sizeof(struct avail);

    /* Initialize K */
    size_t k = 0;
    size_t temp = bytes - 1;  // Subtract 1 to handle exact powers of two

    while (temp >>= 1) {
        k++;
    }
    k++;  // Increment to get the next power of two

    /* Ensure that K is at least SMALLEST_K */
    if (k < SMALLEST_K) {
        k = SMALLEST_K;
    }

    /* Ensure that K does not exceed MAX_K */
    if (k >= MAX_K) {
        k = MAX_K - 1;
    }

    return k;
}

/**
 * Find the buddy of a given pointer and kval relative to the base address we got from mmap
 * @param pool The memory pool to work on (needed for the base addresses)
 * @param block The memory block that we want to find the buddy for
 * @return A pointer to the buddy
 */
struct avail *buddy_calc(struct buddy_pool *pool, struct avail *block) {
    /* Notes:
     - The buddy address can be calculated by flipping the kth bit of the block's relative address.
     - Since block addresses are aligned, we can use bitwise XOR to find the buddy.
    */

    size_t offset = (char *)block - (char *)pool->base;
    size_t buddy_offset = offset ^ (1UL << block->kval);
    struct avail *buddy = (struct avail *)((char *)pool->base + buddy_offset);

    return buddy;
}

/**
 * Allocates a block of size bytes of memory, returning a pointer to
 * the beginning of the block.
 *
 * @param pool The memory pool to alloc from
 * @param size The size of the user requested memory block in bytes
 * @return A pointer to the memory block
 */
void *buddy_malloc(struct buddy_pool *pool, size_t size) {
    /* Notes:
     - Check for invalid inputs (NULL pool or zero size).
     - Calculate the required K value using btok.
     - Find the smallest available block that can accommodate the requested size.
     - If such a block is found, split it as necessary.
     - Update the avail list and block headers accordingly.
    */

    if (pool == NULL || size == 0) {
        return NULL;
    }

    size_t k = btok(size);
    if (k > pool->kval_m) {
        errno = ENOMEM;
        return NULL;
    }

    size_t i;
    for (i = k; i <= pool->kval_m; i++) {
        if (pool->avail[i].next != &pool->avail[i]) {
            break;  // Found an available block
        }
    }

    if (i > pool->kval_m) {
        errno = ENOMEM;
        return NULL;  // No suitable block found
    }

    // Remove block from avail list
    struct avail *block = pool->avail[i].next;
    block->prev->next = block->next;
    block->next->prev = block->prev;

    // Split blocks as necessary
    while (i > k) {
        i--;
        size_t split_size = 1UL << i;
        struct avail *buddy = (struct avail *)((char *)block + split_size);

        // Initialize buddy header
        buddy->tag = BLOCK_AVAIL;
        buddy->kval = i;

        // Add buddy to avail list
        buddy->next = pool->avail[i].next;
        buddy->prev = &pool->avail[i];
        pool->avail[i].next->prev = buddy;
        pool->avail[i].next = buddy;
    }

    // Mark the block as reserved
    block->tag = BLOCK_RESERVED;
    block->kval = k;

    // Return pointer to the memory after the header
    return (void *)(block + 1);
}

/**
 * Frees a block of memory previously allocated by buddy_malloc.
 *
 * @param pool The memory pool
 * @param ptr Pointer to the memory block to free
 */
void buddy_free(struct buddy_pool *pool, void *ptr) {
    /* Notes:
     - Check for invalid inputs (NULL pool or ptr).
     - Retrieve the block header from the given pointer.
     - Attempt to coalesce the block with its buddy recursively.
     - Update the avail list and block headers accordingly.
    */

    if (pool == NULL || ptr == NULL) {
        return;
    }

    struct avail *block = (struct avail *)ptr - 1;
    size_t k = block->kval;

    // Mark the block as available
    block->tag = BLOCK_AVAIL;

    // Coalescing loop
    while (k < pool->kval_m) {
        struct avail *buddy = buddy_calc(pool, block);

        if (buddy->tag != BLOCK_AVAIL || buddy->kval != k) {
            // Cannot coalesce
            break;
        }

        // Remove buddy from avail list
        buddy->prev->next = buddy->next;
        buddy->next->prev = buddy->prev;

        // Determine the lower address
        if (block > buddy) {
            struct avail *temp = block;
            block = buddy;
            buddy = temp;
        }

        // Update block's kval
        k++;
        block->kval = k;
    }

    // Add the (possibly coalesced) block back to avail list
    block->next = pool->avail[k].next;
    block->prev = &pool->avail[k];
    pool->avail[k].next->prev = block;
    pool->avail[k].next = block;
}

/**
 * Reallocates a memory block to a new size.
 *
 * @param pool The memory pool
 * @param ptr Pointer to a memory block
 * @param size The new size of the memory block
 * @return Pointer to the new memory block
 */
void *buddy_realloc(struct buddy_pool *pool, void *ptr, size_t size) {
    /* Notes:
     - If ptr is NULL, behave like malloc.
     - If size is zero, free the block and return NULL.
     - Otherwise, attempt to allocate a new block and copy the data.
     - Free the old block.
    */

    if (ptr == NULL) {
        return buddy_malloc(pool, size);
    }

    if (size == 0) {
        buddy_free(pool, ptr);
        return NULL;
    }

    struct avail *block = (struct avail *)ptr - 1;
    size_t old_size = (1UL << block->kval) - sizeof(struct avail);
    size_t new_size = size;

    if (new_size <= old_size) {
        // The current block is sufficient
        return ptr;
    } else {
        // Need to allocate a new block
        void *new_ptr = buddy_malloc(pool, size);
        if (new_ptr == NULL) {
            return NULL;  // Allocation failed
        }

        // Copy the data to the new block
        memcpy(new_ptr, ptr, old_size);

        // Free the old block
        buddy_free(pool, ptr);

        return new_ptr;
    }
}

/**
 * Initializes a new memory pool using the buddy algorithm.
 *
 * @param pool A pointer to the pool to initialize
 * @param size The size of the pool in bytes
 */
void buddy_init(struct buddy_pool *pool, size_t size) {
    /* Notes:
     - Determine the maximum K value (kval_m) based on the requested size.
     - Ensure that kval_m is within MIN_K and MAX_K - 1.
     - Allocate the memory pool using mmap.
     - Initialize the avail list and the initial block.
    */

    if (size == 0) {
        pool->kval_m = DEFAULT_K;
    } else {
        size_t k = btok(size);
        if (k < MIN_K) {
            k = MIN_K;
        }
        if (k >= MAX_K) {
            k = MAX_K - 1;
        }
        pool->kval_m = k;
    }

    pool->numbytes = 1UL << pool->kval_m;

    // Allocate memory using mmap
    pool->base = mmap(NULL, pool->numbytes, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (pool->base == MAP_FAILED) {
        perror("mmap failed");
        pool->base = NULL;
        return;
    }

    // Initialize the avail array
    for (size_t i = 0; i < MAX_K; i++) {
        pool->avail[i].tag = BLOCK_UNUSED;
        pool->avail[i].kval = i;
        pool->avail[i].next = &pool->avail[i];
        pool->avail[i].prev = &pool->avail[i];
    }

    // Initialize the initial block
    struct avail *initial_block = (struct avail *)pool->base;
    initial_block->tag = BLOCK_AVAIL;
    initial_block->kval = pool->kval_m;

    // Add initial block to avail list
    initial_block->next = &pool->avail[pool->kval_m];
    initial_block->prev = &pool->avail[pool->kval_m];
    pool->avail[pool->kval_m].next = initial_block;
    pool->avail[pool->kval_m].prev = initial_block;
}

/**
 * Destroys a memory pool previously initialized by buddy_init.
 *
 * @param pool The memory pool to destroy
 */
void buddy_destroy(struct buddy_pool *pool) {
    /* Notes:
     - Check if pool and pool->base are valid.
     - Use munmap to deallocate the memory.
     - Set pool->base to NULL to indicate that it's no longer valid.
    */

    if (pool == NULL || pool->base == NULL) {
        return;
    }

    munmap(pool->base, pool->numbytes);
    pool->base = NULL;
}

/**
 * Entry to a main function for testing purposes
 *
 * @param argc system argc
 * @param argv system argv
 * @return exit status
 */
int myMain(int argc, char **argv) {
    /* Notes:
     - Since main.c is empty and no specific instructions were given, this function can remain empty.
     - It could be used for custom tests or examples if needed.
    */

    printf("myMain function is not implemented.\n");
    return 0;
}
