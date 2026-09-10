#include "uaccess.h"
#include "paging.h"
#include "irq.h"

int ua_valid(vaddr_t va, usize_t len) {
    if (va < USER_VA_MIN) return 0;
    if (va >= USER_VA_MAX) return 0;
    if (len > USER_VA_MAX - va) return 0;
    return 1;
}

int ua_copy_from(paddr_t pd_phys, void *kdst, vaddr_t usrc, usize_t len) {
    u8 *dst = (u8 *)kdst;
    if (!ua_valid(usrc, len)) return -1;
    irq_state_t state = irq_save();
    usize_t done = 0;
    while (done < len) {
        vaddr_t va = usrc + done;
        paddr_t phys = paging_user_phys(pd_phys, va);
        if (!phys) {
            irq_restore(state);
            return -1;
        }
        usize_t off = va & 0xFFF;
        usize_t count = 4096 - off;
        if (count > len - done) count = len - done;
        u8 *src = (u8 *)paging_kmap_b(phys) + off;
        for (usize_t i = 0; i < count; i++) dst[done + i] = src[i];
        done += count;
    }
    irq_restore(state);
    return 0;
}

int ua_copy_to(paddr_t pd_phys, vaddr_t udst, const void *ksrc, usize_t len) {
    const u8 *src = (const u8 *)ksrc;
    if (!ua_valid(udst, len)) return -1;
    irq_state_t state = irq_save();
    usize_t done = 0;
    while (done < len) {
        vaddr_t va = udst + done;
        paddr_t phys = paging_user_phys(pd_phys, va);
        if (!phys) {
            irq_restore(state);
            return -1;
        }
        if (!paging_user_writable(pd_phys, va)) {
            if (!paging_handle_cow(pd_phys, va)) {
                irq_restore(state);
                return -1;
            }
            phys = paging_user_phys(pd_phys, va);
            if (!phys) {
                irq_restore(state);
                return -1;
            }
        }
        usize_t off = va & 0xFFF;
        usize_t count = 4096 - off;
        if (count > len - done) count = len - done;
        u8 *dst = (u8 *)paging_kmap_b(phys) + off;
        for (usize_t i = 0; i < count; i++) dst[i] = src[done + i];
        done += count;
    }
    irq_restore(state);
    return 0;
}

int ua_copy_str(paddr_t pd_phys, char *kdst, vaddr_t usrc, usize_t max) {
    if (!max) return -1;
    irq_state_t state = irq_save();
    vaddr_t page_base = 0;
    u8 *src = 0;
    for (usize_t i = 0; i + 1 < max; i++) {
        vaddr_t va = usrc + i;
        if (!ua_valid(va, 1)) {
            irq_restore(state);
            return -1;
        }
        if (!src || (va & ~(vaddr_t)0xFFF) != page_base) {
            paddr_t phys = paging_user_phys(pd_phys, va);
            if (!phys) {
                irq_restore(state);
                return -1;
            }
            src = (u8 *)paging_kmap_b(phys);
            page_base = va & ~(vaddr_t)0xFFF;
        }
        u8 value = src[va & 0xFFF];
        kdst[i] = (char)value;
        if (!value) {
            irq_restore(state);
            return (int)i;
        }
    }
    kdst[max - 1] = 0;
    irq_restore(state);
    return (int)(max - 1);
}
