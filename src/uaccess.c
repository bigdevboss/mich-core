#include "uaccess.h"
#include "paging.h"

int ua_valid(unsigned int va, unsigned int len) {
    if (va < USER_VA_MIN) return 0;
    if (va >= USER_VA_MAX) return 0;
    if (len > USER_VA_MAX - va) return 0;
    return 1;
}

int ua_copy_from(unsigned int pd_phys, void *kdst, unsigned int usrc, unsigned int len) {
    unsigned char *d = (unsigned char *)kdst;
    if (!ua_valid(usrc, len)) return -1;
    unsigned int done = 0;
    while (done < len) {
        unsigned int va = usrc + done;
        unsigned int phys = paging_user_phys(pd_phys, va);
        if (!phys) return -1;
        unsigned int off = va & 0xFFF;
        unsigned int n = 4096 - off;
        if (n > len - done) n = len - done;
        unsigned char *s = (unsigned char *)paging_kmap_b(phys) + off;
        for (unsigned int i = 0; i < n; i++) d[done + i] = s[i];
        done += n;
    }
    return 0;
}

int ua_copy_to(unsigned int pd_phys, unsigned int udst, const void *ksrc, unsigned int len) {
    const unsigned char *s = (const unsigned char *)ksrc;
    if (!ua_valid(udst, len)) return -1;
    unsigned int done = 0;
    while (done < len) {
        unsigned int va = udst + done;
        unsigned int phys = paging_user_phys(pd_phys, va);
        if (!phys) return -1;
        unsigned int off = va & 0xFFF;
        unsigned int n = 4096 - off;
        if (n > len - done) n = len - done;
        unsigned char *d = (unsigned char *)paging_kmap_b(phys) + off;
        for (unsigned int i = 0; i < n; i++) d[i] = s[done + i];
        done += n;
    }
    return 0;
}

int ua_copy_str(unsigned int pd_phys, char *kdst, unsigned int usrc, unsigned int max) {
    if (max == 0) return -1;
    unsigned int page_base = 0;
    unsigned char *s = 0;
    for (unsigned int i = 0; i + 1 < max; i++) {
        unsigned int va = usrc + i;
        if (!ua_valid(va, 1)) return -1;
        if (!s || (va & 0xFFFFF000) != page_base) {
            unsigned int phys = paging_user_phys(pd_phys, va);
            if (!phys) return -1;
            s = (unsigned char *)paging_kmap_b(phys);
            page_base = va & 0xFFFFF000;
        }
        unsigned char c = s[va & 0xFFF];
        kdst[i] = (char)c;
        if (c == 0) return (int)i;
    }
    kdst[max - 1] = 0;
    return (int)(max - 1);
}
