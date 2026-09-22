#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <mich/syscall.h>

/* Page-mapped heap: blocks live in the arena between the heap base and the
   program break, walked in address order, so coalescing only needs the two
   physical neighbors of a freed block. */
#define MALLOC_ALIGN 16
#define MALLOC_MIN_PAYLOAD 16
#define MALLOC_GROW 65536

struct malloc_block {
    size_t size;
    int used;
};

static size_t heap_base;
static size_t heap_break;
static int heap_ready;

static int heap_init(void) {
    if (heap_ready) return 0;
    unsigned long base =
        (unsigned long)mich_syscall1(MICH_SYS_POSIX_BRK, 0);
    if (!base) return -1;
    heap_base = base;
    heap_break = base;
    heap_ready = 1;
    return 0;
}

static struct malloc_block *block_after(struct malloc_block *block) {
    return (struct malloc_block *)((char *)block + sizeof(*block) +
                                   block->size);
}

static void heap_split(struct malloc_block *block, size_t payload) {
    if (block->size >= payload + sizeof(struct malloc_block) +
                       MALLOC_MIN_PAYLOAD) {
        struct malloc_block *tail =
            (struct malloc_block *)((char *)block + sizeof(*block) + payload);
        tail->size = block->size - payload - sizeof(struct malloc_block);
        tail->used = 0;
        block->size = payload;
    }
}

static struct malloc_block *heap_extend(size_t payload) {
    size_t total = payload + sizeof(struct malloc_block);
    size_t chunk = total < MALLOC_GROW ? MALLOC_GROW : total;
    /* The break syscall rejects growth past the bounded heap window by
       returning the unchanged break, which this compare turns into failure. */
    if (mich_syscall1(MICH_SYS_POSIX_BRK, (unsigned long)(heap_break + chunk)) !=
        (long)(heap_break + chunk))
        return 0;
    struct malloc_block *block = (struct malloc_block *)heap_break;
    heap_break += chunk;
    block->size = chunk - sizeof(struct malloc_block);
    block->used = 0;
    return block;
}

static void heap_coalesce(struct malloc_block *block,
                          struct malloc_block *previous) {
    struct malloc_block *next = block_after(block);
    if ((size_t)next < heap_break && !next->used)
        block->size += sizeof(struct malloc_block) + next->size;
    if (previous && !previous->used)
        previous->size += sizeof(struct malloc_block) + block->size;
}

void *malloc(size_t size) {
    if (!size || heap_init()) {
        errno = ENOMEM;
        return 0;
    }
    size = (size + MALLOC_ALIGN - 1) & ~(size_t)(MALLOC_ALIGN - 1);
    struct malloc_block *found = 0;
    for (struct malloc_block *block = (struct malloc_block *)heap_base;
         (size_t)block < heap_break;
         block = block_after(block)) {
        if (!block->used && block->size >= size) {
            found = block;
            break;
        }
    }
    if (!found) {
        found = heap_extend(size);
        if (!found) {
            errno = ENOMEM;
            return 0;
        }
    }
    heap_split(found, size);
    found->used = 1;
    return (char *)found + sizeof(struct malloc_block);
}

void free(void *pointer) {
    if (!pointer || heap_init()) return;
    struct malloc_block *previous = 0;
    for (struct malloc_block *block = (struct malloc_block *)heap_base;
         (size_t)block < heap_break;
         previous = block, block = block_after(block)) {
        if ((char *)block + sizeof(struct malloc_block) == pointer) {
            /* Releasing an already free block would corrupt its neighbors,
               so a double free is ignored instead of merged twice. */
            if (block->used) {
                block->used = 0;
                heap_coalesce(block, previous);
            }
            return;
        }
    }
}

void *calloc(size_t count, size_t size) {
    if (count && size > (size_t)-1 / count) {
        errno = ENOMEM;
        return 0;
    }
    void *pointer = malloc(count * size);
    if (pointer) memset(pointer, 0, count * size);
    return pointer;
}

void *realloc(void *pointer, size_t size) {
    if (!pointer) return malloc(size);
    if (!size) {
        free(pointer);
        return 0;
    }
    if (heap_init()) return 0;
    size_t payload = (size + MALLOC_ALIGN - 1) & ~(size_t)(MALLOC_ALIGN - 1);
    if ((size_t)pointer < heap_base + sizeof(struct malloc_block) ||
        (size_t)pointer > heap_break) {
        errno = EINVAL;
        return 0;
    }
    struct malloc_block *block =
        (struct malloc_block *)((char *)pointer - sizeof(struct malloc_block));
    if (block->size >= payload) {
        heap_split(block, payload);
        return pointer;
    }
    struct malloc_block *next = block_after(block);
    if ((size_t)next < heap_break && !next->used &&
        block->size + sizeof(struct malloc_block) + next->size >= payload) {
        block->size += sizeof(struct malloc_block) + next->size;
        return pointer;
    }
    void *fresh = malloc(size);
    if (!fresh) return 0;
    memcpy(fresh, pointer, block->size);
    free(pointer);
    return fresh;
}
