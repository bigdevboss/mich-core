#include "elf.h"
#include "paging.h"
#include "pmm.h"
#include "task.h"

#define ELFCLASS32 1
#define ELFDATA2LSB 1
#define ET_EXEC 2
#define EM_386 3
#define PT_LOAD 1
#define ELF_PHNUM_MAX 32
#define ELF_SEG_BYTES_MAX 0x4000000U

#define USER_STACK_VADDR 0x1080000

struct elf32_header {
    unsigned char e_ident[16];
    unsigned short e_type;
    unsigned short e_machine;
    unsigned int e_version;
    unsigned int e_entry;
    unsigned int e_phoff;
    unsigned int e_shoff;
    unsigned int e_flags;
    unsigned short e_ehsize;
    unsigned short e_phentsize;
    unsigned short e_phnum;
    unsigned short e_shentsize;
    unsigned short e_shnum;
    unsigned short e_shstrndx;
};

struct elf32_phdr {
    unsigned int p_type;
    unsigned int p_offset;
    unsigned int p_vaddr;
    unsigned int p_paddr;
    unsigned int p_filesz;
    unsigned int p_memsz;
    unsigned int p_flags;
    unsigned int p_align;
};

static int elf_bad_vaddr(unsigned int vaddr, unsigned int memsz) {
    if (vaddr < USER_VA_MIN) return 1;
    if (vaddr >= USER_VA_MAX) return 1;
    if (memsz > USER_VA_MAX - vaddr) return 1;
    if (vaddr & 0xFFF) return 1;
    return 0;
}

int elf_map(unsigned int pd_phys, const unsigned char *img, unsigned int size, unsigned int *entry_out) {
    if (size < sizeof(struct elf32_header)) return -1;
    struct elf32_header *header = (struct elf32_header *)img;
    if (header->e_ident[0] != 0x7F || header->e_ident[1] != 'E' ||
        header->e_ident[2] != 'L' || header->e_ident[3] != 'F')
        return -1;
    if (header->e_ident[4] != ELFCLASS32 || header->e_ident[5] != ELFDATA2LSB)
        return -1;
    if (header->e_type != ET_EXEC || header->e_machine != EM_386)
        return -1;
    if (header->e_phnum == 0 || header->e_phnum > ELF_PHNUM_MAX)
        return -1;
    if (header->e_phentsize < sizeof(struct elf32_phdr))
        return -1;
    if ((unsigned long long)header->e_phoff +
        (unsigned long long)header->e_phnum * header->e_phentsize > size)
        return -1;

    int entry_ok = 0;
    for (int i = 0; i < header->e_phnum; i++) {
        struct elf32_phdr *phdr = (struct elf32_phdr *)(img + header->e_phoff + i * header->e_phentsize);
        if (phdr->p_type != PT_LOAD) continue;
        if (phdr->p_memsz == 0) continue;
        if ((unsigned long long)phdr->p_offset + phdr->p_filesz > size) return -1;
        if (phdr->p_filesz > phdr->p_memsz) return -1;
        if (phdr->p_memsz > ELF_SEG_BYTES_MAX) return -1;
        if (elf_bad_vaddr(phdr->p_vaddr, phdr->p_memsz)) return -1;
        if (phdr->p_offset & 0xFFF) return -1;
        if (header->e_entry >= phdr->p_vaddr &&
            header->e_entry < phdr->p_vaddr + phdr->p_memsz)
            entry_ok = 1;
    }
    if (!entry_ok) return -1;

    for (int i = 0; i < header->e_phnum; i++) {
        struct elf32_phdr *phdr = (struct elf32_phdr *)(img + header->e_phoff + i * header->e_phentsize);
        if (phdr->p_type != PT_LOAD) continue;
        if (phdr->p_memsz == 0) continue;

        for (unsigned int offset = 0; offset < phdr->p_memsz; offset += 4096) {
            unsigned int phys = pmm_alloc_page();
            if (!phys) return -2;
            paging_map_user(pd_phys, phys, phdr->p_vaddr + offset, USER_FLAGS);
            unsigned int copy = 0;
            if (offset < phdr->p_filesz) {
                copy = 4096;
                if (offset + copy > phdr->p_filesz) copy = phdr->p_filesz - offset;
            }
            unsigned char *dst = (unsigned char *)paging_kmap_b(phys);
            const unsigned char *src = img + phdr->p_offset + offset;
            for (unsigned int j = 0; j < copy; j++) dst[j] = src[j];
            for (unsigned int j = copy; j < 4096; j++) dst[j] = 0;
        }
    }
    *entry_out = header->e_entry;
    return 0;
}

int elf_load(unsigned int *module_addr, unsigned int module_size, const char *name) {
    unsigned int pd_phys = pmm_alloc_page_low();
    if (!pd_phys) return -2;
    paging_create_user_directory(pd_phys);

    unsigned int entry = 0;
    if (elf_map(pd_phys, (const unsigned char *)module_addr, module_size, &entry) != 0) {
        pmm_free_page(pd_phys);
        return -1;
    }

    unsigned int ustack_phys = pmm_alloc_page();
    unsigned int kstack_phys = pmm_alloc_page_low();
    if (!ustack_phys || !kstack_phys) return -2;
    paging_map_user(pd_phys, ustack_phys, USER_STACK_VADDR, USER_FLAGS);

    struct task *t = create_task((void(*)(void))entry,
                                 (unsigned int *)(USER_STACK_VADDR + 4096),
                                 (unsigned int *)(kstack_phys + 4096), 3);
    if (!t) return -3;
    t->page_dir = pd_phys;
    t->kstack_phys = kstack_phys;
    t->parent_id = 0;
    task_set_name(t, name);
    return 0;
}
