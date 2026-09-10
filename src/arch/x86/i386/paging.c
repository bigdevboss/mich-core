#include "paging.h"
#include "pmm.h"
#include "protos.h"

#define PAGE_SIZE 4096
#define PAGE_TABLE_ENTRIES 1024
#define PAGE_DIR_ENTRIES 1024
#define PAGING_FLAG_PRESENT 0x1
#define PAGING_FLAG_WRITABLE 0x2
#define PAGING_FLAG_USER 0x4

#define USER_PD_INDEX_START 4

#define SCRATCH_VA_A 0x90000000
#define SCRATCH_VA_B 0x90001000

static unsigned int page_table_0[PAGE_TABLE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static unsigned int page_table_1[PAGE_TABLE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static unsigned int page_table_2[PAGE_TABLE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static unsigned int page_table_3[PAGE_TABLE_ENTRIES] __attribute__((aligned(PAGE_SIZE)));

static unsigned int page_directory[PAGE_DIR_ENTRIES] __attribute__((aligned(PAGE_SIZE)));

extern char _text_start;
extern char _text_end;
extern char _rodata_start;
extern char _rodata_end;

static void paging_identity_readonly(unsigned int start, unsigned int end) {
    start &= 0xFFFFF000u;
    end = (end + PAGE_SIZE - 1) & 0xFFFFF000u;
    for (unsigned int va = start; va < end; va += PAGE_SIZE) {
        unsigned int pd_index = va >> 22;
        unsigned int pt_index = (va >> 12) & 0x3FF;
        unsigned int *pt = (unsigned int *)(page_directory[pd_index] & 0xFFFFF000u);
        pt[pt_index] &= ~PAGING_FLAG_WRITABLE;
    }
}

unsigned int paging_kernel_dir_phys(void) {
    return (unsigned int)page_directory;
}

static void invlpg(unsigned int va) {
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

static unsigned int *scratch_pt(void) {
    return (unsigned int *)(page_directory[SCRATCH_VA_A >> 22] & 0xFFFFF000);
}

static void scratch_require_locked(void) {
    unsigned int flags;
    __asm__ volatile("pushf; pop %0" : "=r"(flags));
    if (flags & (1u << 9)) panic_str("scratch mapping without IRQ lock");
}

unsigned int *paging_kmap_a(unsigned int phys) {
    scratch_require_locked();
    scratch_pt()[(SCRATCH_VA_A >> 12) & 0x3FF] = (phys & 0xFFFFF000) | 0x3;
    invlpg(SCRATCH_VA_A);
    return (unsigned int *)SCRATCH_VA_A;
}

unsigned int *paging_kmap_b(unsigned int phys) {
    scratch_require_locked();
    scratch_pt()[(SCRATCH_VA_B >> 12) & 0x3FF] = (phys & 0xFFFFF000) | 0x3;
    invlpg(SCRATCH_VA_B);
    return (unsigned int *)SCRATCH_VA_B;
}

void paging_init(void) {
    for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) {
        page_table_0[i] = (i * PAGE_SIZE) | PAGING_FLAG_PRESENT | PAGING_FLAG_WRITABLE;
        page_table_1[i] = ((1024 + i) * PAGE_SIZE) | PAGING_FLAG_PRESENT | PAGING_FLAG_WRITABLE;
        page_table_2[i] = ((2048 + i) * PAGE_SIZE) | PAGING_FLAG_PRESENT | PAGING_FLAG_WRITABLE;
        page_table_3[i] = ((3072 + i) * PAGE_SIZE) | PAGING_FLAG_PRESENT | PAGING_FLAG_WRITABLE;
    }

    for (int i = 0; i < PAGE_DIR_ENTRIES; i++) {
        page_directory[i] = 0;
    }

    page_directory[0] = (unsigned int)page_table_0 | PAGING_FLAG_PRESENT | PAGING_FLAG_WRITABLE;
    page_directory[1] = (unsigned int)page_table_1 | PAGING_FLAG_PRESENT | PAGING_FLAG_WRITABLE;
    page_directory[2] = (unsigned int)page_table_2 | PAGING_FLAG_PRESENT | PAGING_FLAG_WRITABLE;
    page_directory[3] = (unsigned int)page_table_3 | PAGING_FLAG_PRESENT | PAGING_FLAG_WRITABLE;

    paging_identity_readonly((unsigned int)&_text_start, (unsigned int)&_text_end);
    paging_identity_readonly((unsigned int)&_rodata_start, (unsigned int)&_rodata_end);
}

void paging_init_scratch(void) {
    if (paging_map_kernel(0, SCRATCH_VA_A, PAGING_FLAG_WRITABLE) != 0)
        panic_str("scratch map failed");
    if (paging_map_kernel(0, SCRATCH_VA_B, PAGING_FLAG_WRITABLE) != 0)
        panic_str("scratch map failed");
    scratch_pt()[(SCRATCH_VA_A >> 12) & 0x3FF] = 0;
    scratch_pt()[(SCRATCH_VA_B >> 12) & 0x3FF] = 0;
    invlpg(SCRATCH_VA_A);
    invlpg(SCRATCH_VA_B);
}

int paging_map_kernel(unsigned int phys, unsigned int virt, unsigned int flags) {
    unsigned int pd_index = virt >> 22;
    unsigned int pt_index = (virt >> 12) & 0x3FF;

    if (!(page_directory[pd_index] & PAGING_FLAG_PRESENT)) {
        unsigned int pt_phys = pmm_alloc_page_low();
        if (!pt_phys) return -1;
        page_directory[pd_index] = pt_phys | PAGING_FLAG_PRESENT | PAGING_FLAG_WRITABLE;
    }

    unsigned int *pt = (unsigned int *)(page_directory[pd_index] & 0xFFFFF000);
    pt[pt_index] = (phys & 0xFFFFF000) | (flags & 0xFFF) | PAGING_FLAG_PRESENT;
    return 0;
}

int paging_map_user(unsigned int pd_phys, unsigned int phys, unsigned int virt, unsigned int flags) {
    unsigned int pd_index = virt >> 22;
    unsigned int pt_index = (virt >> 12) & 0x3FF;
    unsigned int pt_phys;

    if (pd_index < USER_PD_INDEX_START || pd_index >= 512) return -1;

    unsigned int *pd = paging_kmap_a(pd_phys);
    if (!(pd[pd_index] & PAGING_FLAG_PRESENT)) {
        pt_phys = pmm_alloc_page_low();
        if (!pt_phys) return -1;
        pd[pd_index] = pt_phys | PAGING_FLAG_PRESENT | PAGING_FLAG_WRITABLE | PAGING_FLAG_USER;
        unsigned int *pt = paging_kmap_b(pt_phys);
        for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) pt[i] = 0;
    } else {
        pt_phys = pd[pd_index] & 0xFFFFF000;
    }

    unsigned int *pt = paging_kmap_b(pt_phys);
    if (pt[pt_index] & PAGING_FLAG_PRESENT) return -1;
    pt[pt_index] = (phys & 0xFFFFF000) | (flags & 0xFFF) | PAGING_FLAG_PRESENT;
    return 0;
}

void paging_create_user_directory(unsigned int pd_phys) {
    unsigned int *user_pd = paging_kmap_b(pd_phys);
    for (int i = 0; i < PAGE_DIR_ENTRIES; i++) {
        if (i < USER_PD_INDEX_START) {
            user_pd[i] = page_directory[i];
        } else if (i >= 512 && (page_directory[i] & PAGING_FLAG_PRESENT)) {
            user_pd[i] = page_directory[i];
        } else {
            user_pd[i] = 0;
        }
    }
}

unsigned int paging_clone_user(unsigned int dst_pd_phys, unsigned int src_pd_phys) {
    for (int i = USER_PD_INDEX_START; i < 512; i++) {
        unsigned int *src_pd = paging_kmap_a(src_pd_phys);
        unsigned int src_pd_e = src_pd[i];
        if (!(src_pd_e & PAGING_FLAG_PRESENT)) continue;
        unsigned int src_pt_phys = src_pd_e & 0xFFFFF000;

        unsigned int *dst_pd = paging_kmap_b(dst_pd_phys);
        unsigned int dst_pt_phys;
        if (!(dst_pd[i] & PAGING_FLAG_PRESENT)) {
            dst_pt_phys = pmm_alloc_page_low();
            if (!dst_pt_phys) return 0;
            dst_pd[i] = dst_pt_phys | (src_pd_e & 0xFFF);
            unsigned int *dpt = paging_kmap_b(dst_pt_phys);
            for (int k = 0; k < PAGE_TABLE_ENTRIES; k++) dpt[k] = 0;
        } else {
            dst_pt_phys = dst_pd[i] & 0xFFFFF000;
        }

        for (int j = 0; j < PAGE_TABLE_ENTRIES; j++) {
            unsigned int *src_pt = paging_kmap_a(src_pt_phys);
            unsigned int sp = src_pt[j];
            if (!(sp & PAGING_FLAG_PRESENT)) continue;
            if (!(sp & PAGE_FLAG_OWNED)) continue;

            unsigned int phys = sp & 0xFFFFF000;
            unsigned int flags = sp & 0xFFF;
            if (flags & PAGING_FLAG_WRITABLE) {
                flags = (flags & ~PAGING_FLAG_WRITABLE) | PAGE_FLAG_COW;
                src_pt[j] = phys | flags;
                invlpg(((unsigned int)i << 22) | ((unsigned int)j << 12));
            }
            if (pmm_retain_page(phys) != 0) return 0;
            unsigned int *dst_pt = paging_kmap_b(dst_pt_phys);
            dst_pt[j] = phys | flags;
        }
    }
    return 1;
}

int paging_handle_cow(unsigned int pd_phys, unsigned int va) {
    unsigned int pd_index = va >> 22;
    unsigned int pt_index = (va >> 12) & 0x3FF;
    unsigned int *pd = paging_kmap_a(pd_phys);
    unsigned int pde = pd[pd_index];
    if (!(pde & PAGING_FLAG_PRESENT)) return 0;
    unsigned int pt_phys = pde & 0xFFFFF000;
    unsigned int *pt = paging_kmap_a(pt_phys);
    unsigned int old = pt[pt_index];
    if (!(old & PAGING_FLAG_PRESENT) || !(old & PAGE_FLAG_COW) ||
        !(old & PAGE_FLAG_OWNED))
        return 0;

    unsigned int old_phys = old & 0xFFFFF000;
    unsigned int flags = (old & 0xFFF) | PAGING_FLAG_WRITABLE;
    flags &= ~PAGE_FLAG_COW;
    unsigned int refs = pmm_page_refs(old_phys);
    if (!refs) return 0;
    if (refs == 1) {
        pt[pt_index] = old_phys | flags;
        invlpg(va & 0xFFFFF000);
        return 1;
    }

    unsigned int new_phys = pmm_alloc_page();
    if (!new_phys) return 0;
    unsigned int *from = paging_kmap_a(old_phys);
    unsigned int *to = paging_kmap_b(new_phys);
    for (unsigned int i = 0; i < PAGE_SIZE / 4; i++) to[i] = from[i];
    pt = paging_kmap_a(pt_phys);
    pt[pt_index] = new_phys | flags;
    pmm_free_page(old_phys);
    invlpg(va & 0xFFFFF000);
    return 1;
}

int paging_unmap_user(unsigned int pd_phys, unsigned int va) {
    unsigned int pd_index = va >> 22;
    unsigned int pt_index = (va >> 12) & 0x3FF;
    unsigned int *pd = paging_kmap_a(pd_phys);
    unsigned int pde = pd[pd_index];
    if (!(pde & PAGING_FLAG_PRESENT)) return -1;
    unsigned int *pt = paging_kmap_b(pde & 0xFFFFF000);
    if (!(pt[pt_index] & PAGING_FLAG_PRESENT)) return -1;
    pt[pt_index] = 0;
    invlpg(va & 0xFFFFF000);
    return 0;
}

int paging_user_writable(unsigned int pd_phys, unsigned int va) {
    unsigned int pd_index = va >> 22;
    unsigned int pt_index = (va >> 12) & 0x3FF;
    unsigned int *pd = paging_kmap_a(pd_phys);
    unsigned int pde = pd[pd_index];
    if (!(pde & PAGING_FLAG_PRESENT) || !(pde & PAGING_FLAG_USER)) return 0;
    unsigned int *pt = paging_kmap_b(pde & 0xFFFFF000);
    unsigned int pte = pt[pt_index];
    return (pte & (PAGING_FLAG_PRESENT | PAGING_FLAG_USER | PAGING_FLAG_WRITABLE)) ==
           (PAGING_FLAG_PRESENT | PAGING_FLAG_USER | PAGING_FLAG_WRITABLE);
}

unsigned int paging_user_phys(unsigned int pd_phys, unsigned int va) {
    unsigned int pd_index = va >> 22;
    unsigned int pt_index = (va >> 12) & 0x3FF;
    unsigned int *pd = paging_kmap_a(pd_phys);
    unsigned int pde = pd[pd_index];
    if (!(pde & PAGING_FLAG_PRESENT)) return 0;
    unsigned int *pt = paging_kmap_b(pde & 0xFFFFF000);
    unsigned int pte = pt[pt_index];
    if (!(pte & PAGING_FLAG_PRESENT)) return 0;
    if (!(pte & PAGING_FLAG_USER)) return 0;
    return pte & 0xFFFFF000;
}

void paging_free_user_pages(unsigned int pd_phys) {
    for (int i = USER_PD_INDEX_START; i < 512; i++) {
        unsigned int *pd = paging_kmap_a(pd_phys);
        unsigned int pd_e = pd[i];
        if (!(pd_e & PAGING_FLAG_PRESENT)) continue;
        unsigned int pt_phys = pd_e & 0xFFFFF000;
        pd[i] = 0;

        unsigned int *pt = paging_kmap_a(pt_phys);
        for (int j = 0; j < PAGE_TABLE_ENTRIES; j++) {
            unsigned int pte = pt[j];
            if (pte & PAGING_FLAG_PRESENT) {
                pt[j] = 0;
                if (pte & PAGE_FLAG_OWNED)
                    pmm_free_page(pte & 0xFFFFF000);
            }
        }
        pmm_free_page(pt_phys);
    }
}

void paging_enable(void) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(page_directory));
    unsigned int cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80010000u;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}
