#ifndef UACCESS_H
#define UACCESS_H

int ua_valid(unsigned int va, unsigned int len);
int ua_copy_from(unsigned int pd_phys, void *kdst, unsigned int usrc, unsigned int len);
int ua_copy_to(unsigned int pd_phys, unsigned int udst, const void *ksrc, unsigned int len);
int ua_copy_str(unsigned int pd_phys, char *kdst, unsigned int usrc, unsigned int max);

#endif
