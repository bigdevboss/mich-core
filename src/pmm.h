#ifndef PMM_H
#define PMM_H

void pmm_init(unsigned long mmap_addr, unsigned long mmap_length, unsigned int entry_size, unsigned int kernel_end);
void pmm_reserve(unsigned long addr, unsigned long len);
unsigned int pmm_alloc_page(void);
unsigned int pmm_alloc_page_low(void);
void pmm_free_page(unsigned int addr);
unsigned int pmm_free_pages(void);

#endif
