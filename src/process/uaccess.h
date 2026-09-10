#ifndef UACCESS_H
#define UACCESS_H

#include "types.h"

int ua_valid(vaddr_t va, usize_t len);
int ua_copy_from(paddr_t pd_phys, void *kdst, vaddr_t usrc, usize_t len);
int ua_copy_to(paddr_t pd_phys, vaddr_t udst, const void *ksrc, usize_t len);
int ua_copy_str(paddr_t pd_phys, char *kdst, vaddr_t usrc, usize_t max);

#endif
