#ifndef PAGING_H
#define PAGING_H

void paging_init(void);
void paging_init_scratch(void);
void paging_enable(void);
unsigned int paging_kernel_dir_phys(void);
void paging_map_kernel(unsigned int phys, unsigned int virt, unsigned int flags);
void paging_map_user(unsigned int pd_phys, unsigned int phys, unsigned int virt, unsigned int flags);
void paging_create_user_directory(unsigned int pd_phys);
unsigned int paging_clone_user(unsigned int dst_pd_phys, unsigned int src_pd_phys);
void paging_free_user_pages(unsigned int pd_phys);
unsigned int *paging_kmap_a(unsigned int phys);
unsigned int *paging_kmap_b(unsigned int phys);

#endif
