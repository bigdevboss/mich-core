#ifndef VM64_H
#define VM64_H

#include "types.h"

#define VM64_PROGRAM_BASE 0x100000000ULL
#define VM64_PROGRAM_LIMIT 0x100200000ULL
#define VM64_USER_BASE VM64_PROGRAM_BASE
#define VM64_USER_LIMIT 0x120000000ULL
#define VM64_STACK_TOP VM64_PROGRAM_LIMIT
#define VM64_DRIVER_BASE 0x110000000ULL
#define VM64_DRIVER_LIMIT VM64_USER_LIMIT
#define VM64_PAGE_PRESENT 1ULL
#define VM64_PAGE_WRITE 2ULL
#define VM64_PAGE_USER 4ULL
#define VM64_PAGE_COW (1ULL << 10)
#define VM64_PAGE_NX (1ULL << 63)
#define VM64_KERNEL_MMIO_BASE 0xFFFF800000000000ULL
#define VM64_KERNEL_MMIO_SIZE 0x01000000ULL
#define VM64_CACHE_UC 0
#define VM64_CACHE_WC 1
#define VM64_CACHE_WB 3

struct kernel_object;

int vm64_init(void);
void vm64_activate(paddr_t root);
int vm64_create_space(u32 *space_out);
int vm64_clone_space(u32 source, u32 *destination);
int vm64_handle_cow(u32 space, vaddr_t virtual);
void vm64_destroy_space(u32 space);
paddr_t vm64_root(u32 index);
paddr_t vm64_alloc_page(void);
int vm64_free_page(paddr_t phys);
u32 vm64_available_pages(void);
int vm64_map(u32 space, vaddr_t virt, paddr_t phys, int writable, int executable);
u64 vm64_user_flags(u32 space, vaddr_t virt);
int vm64_user_access(paddr_t root, vaddr_t address, usize_t length, int writable);
int vm64_copy_from(paddr_t root, void *kernel_dst, vaddr_t user_src, usize_t length);
int vm64_copy_to(paddr_t root, vaddr_t user_dst, const void *kernel_src, usize_t length);
int vm64_map_object(u32 space, vaddr_t virtual, struct kernel_object *object,
                    paddr_t physical, usize_t length, int writable,
                    u32 cache_mode);
int vm64_map_page_object(u32 space, vaddr_t virtual,
                         struct kernel_object *object, int writable);
int vm64_unmap_object(u32 space, vaddr_t virtual, usize_t length);
u32 vm64_revoke_objects(u32 space, struct kernel_object **objects, u32 count);
int vm64_revoke_object_all(struct kernel_object *object);
u32 vm64_object_mapping_count(u32 space);
void *vm64_ioremap(paddr_t physical, usize_t length, u32 cache_mode);
int vm64_iounmap(void *address, usize_t length);

#endif
