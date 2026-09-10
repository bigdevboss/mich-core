#ifndef PMM_H
#define PMM_H

#include "types.h"

void pmm_init(uptr_t mmap_addr, usize_t mmap_length, u32 entry_size,
              paddr_t kernel_end);
void pmm_reserve(paddr_t addr, usize_t len);
void pmm_set_alloc_limit(paddr_t end);
paddr_t pmm_alloc_page(void);
paddr_t pmm_alloc_page_low(void);
paddr_t pmm_alloc_page_range(paddr_t start, paddr_t end);
paddr_t pmm_alloc_contiguous(u32 pages, paddr_t max_addr);
void pmm_free_page(paddr_t addr);
int pmm_retain_page(paddr_t addr);
u32 pmm_page_refs(paddr_t addr);
u32 pmm_free_pages(void);
int pmm_range_is_ram(paddr_t addr, usize_t len);

#endif
