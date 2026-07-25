#include "paging.h"
#include "pmm.h"

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

unsigned int paging_kernel_dir_phys(void) {
    return (unsigned int)page_directory;
}

static void invlpg(unsigned int va) {
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

static unsigned int *scratch_pt(void) {
    return (unsigned int *)(page_directory[SCRATCH_VA_A >> 22] & 0xFFFFF000);
}

unsigned int *paging_kmap_a(unsigned int phys) {
    scratch_pt()[(SCRATCH_VA_A >> 12) & 0x3FF] = (phys & 0xFFFFF000) | 0x3;
    invlpg(SCRATCH_VA_A);
    return (unsigned int *)SCRATCH_VA_A;
}

unsigned int *paging_kmap_b(unsigned int phys) {
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
}

void paging_init_scratch(void) {
    paging_map_kernel(0, SCRATCH_VA_A, PAGING_FLAG_WRITABLE);
    paging_map_kernel(0, SCRATCH_VA_B, PAGING_FLAG_WRITABLE);
    scratch_pt()[(SCRATCH_VA_A >> 12) & 0x3FF] = 0;
    scratch_pt()[(SCRATCH_VA_B >> 12) & 0x3FF] = 0;
    invlpg(SCRATCH_VA_A);
    invlpg(SCRATCH_VA_B);
}

void paging_map_kernel(unsigned int phys, unsigned int virt, unsigned int flags) {
    unsigned int pd_index = virt >> 22;
    unsigned int pt_index = (virt >> 12) & 0x3FF;

    if (!(page_directory[pd_index] & PAGING_FLAG_PRESENT)) {
        unsigned int pt_phys = pmm_alloc_page_low();
        if (!pt_phys) return;
        page_directory[pd_index] = pt_phys | PAGING_FLAG_PRESENT | PAGING_FLAG_WRITABLE;
    }

    unsigned int *pt = (unsigned int *)(page_directory[pd_index] & 0xFFFFF000);
    pt[pt_index] = (phys & 0xFFFFF000) | (flags & 0xFFF) | PAGING_FLAG_PRESENT;
}

void paging_map_user(unsigned int pd_phys, unsigned int phys, unsigned int virt, unsigned int flags) {
    unsigned int pd_index = virt >> 22;
    unsigned int pt_index = (virt >> 12) & 0x3FF;
    unsigned int pt_phys;

    unsigned int *pd = paging_kmap_a(pd_phys);
    if (!(pd[pd_index] & PAGING_FLAG_PRESENT)) {
        pt_phys = pmm_alloc_page_low();
        if (!pt_phys) return;
        pd[pd_index] = pt_phys | PAGING_FLAG_PRESENT | PAGING_FLAG_WRITABLE | PAGING_FLAG_USER;
        unsigned int *pt = paging_kmap_b(pt_phys);
        for (int i = 0; i < PAGE_TABLE_ENTRIES; i++) pt[i] = 0;
    } else {
        pt_phys = pd[pd_index] & 0xFFFFF000;
    }

    unsigned int *pt = paging_kmap_b(pt_phys);
    pt[pt_index] = (phys & 0xFFFFF000) | (flags & 0xFFF) | PAGING_FLAG_PRESENT;
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

            unsigned int np = pmm_alloc_page();
            if (!np) return 0;
            unsigned int *dst_pt = paging_kmap_b(dst_pt_phys);
            dst_pt[j] = np | (sp & 0xFFF);

            unsigned int *from = paging_kmap_a(sp & 0xFFFFF000);
            unsigned int *to = paging_kmap_b(np);
            for (unsigned int b = 0; b < PAGE_SIZE / 4; b++) to[b] = from[b];
        }
    }
    return 1;
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

void paging_free_user_pages(unsigned int pd_phys) {    for (int i = USER_PD_INDEX_START; i < 512; i++) {
        unsigned int *pd = paging_kmap_a(pd_phys);
        unsigned int pd_e = pd[i];
        if (!(pd_e & PAGING_FLAG_PRESENT)) continue;
        unsigned int pt_phys = pd_e & 0xFFFFF000;

        unsigned int *pt = paging_kmap_a(pt_phys);
        for (int j = 0; j < PAGE_TABLE_ENTRIES; j++) {
            unsigned int pte = pt[j];
            if (pte & PAGING_FLAG_PRESENT) {
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
    cr0 |= 0x80000000;
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0));
}
