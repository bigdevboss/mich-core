#ifndef ELF_H
#define ELF_H

int elf_load(unsigned int *module_addr, unsigned int module_size, const char *name);
int elf_map(unsigned int pd_phys, const unsigned char *img, unsigned int size, unsigned int *entry_out);

#endif
