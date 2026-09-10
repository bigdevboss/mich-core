#ifndef PAGING_H
#define PAGING_H

#include "types.h"

#define USER_VA_MIN 0x01000000U
#define USER_VA_MAX 0x80000000U
#define PAGE_FLAG_OWNED 0x200U
#define PAGE_FLAG_COW 0x400U
#define USER_FLAGS (0x6U | PAGE_FLAG_OWNED)
#define USER_MMIO_FLAGS 0x1EU

void paging_init(void);
void paging_init_scratch(void);
void paging_enable(void);
paddr_t paging_kernel_dir_phys(void);
int paging_map_kernel(paddr_t phys, vaddr_t virt, u32 flags);
int paging_map_user(paddr_t pd_phys, paddr_t phys, vaddr_t virt, u32 flags);
void paging_create_user_directory(paddr_t pd_phys);
u32 paging_clone_user(paddr_t dst_pd_phys, paddr_t src_pd_phys);
void paging_free_user_pages(paddr_t pd_phys);
u32 *paging_kmap_a(paddr_t phys);
u32 *paging_kmap_b(paddr_t phys);
paddr_t paging_user_phys(paddr_t pd_phys, vaddr_t va);
int paging_user_writable(paddr_t pd_phys, vaddr_t va);
int paging_unmap_user(paddr_t pd_phys, vaddr_t va);
int paging_handle_cow(paddr_t pd_phys, vaddr_t va);

#endif
