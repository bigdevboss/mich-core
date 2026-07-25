#include "tss.h"
#include "task.h"

#define MAX_PRIVATE_MAPS 4

struct tss_full tss_full;

static unsigned char default_map[TSS_IOMAP_BYTES + 1];
static unsigned char private_maps[MAX_PRIVATE_MAPS][TSS_IOMAP_BYTES + 1];
static int private_map_count = 0;
static const unsigned char *loaded_map = 0;

static void map_fill(unsigned char *m, unsigned char v) {
    for (int i = 0; i < TSS_IOMAP_BYTES + 1; i++) m[i] = v;
}

static void map_copy(unsigned char *d, const unsigned char *s) {
    for (int i = 0; i < TSS_IOMAP_BYTES + 1; i++) d[i] = s[i];
}

void tss_init(void) {
    unsigned char *p = (unsigned char *)&tss_full.entry;
    for (unsigned int i = 0; i < sizeof(struct tss_entry); i++) p[i] = 0;
    map_fill(tss_full.iomap, 0xFF);
    map_fill(default_map, 0xFF);
    tss_full.entry.ss0 = 0x10;
    tss_full.entry.esp0 = 0x200000 + 4096;
    tss_full.entry.esp_df = 0x210000 + 4096;
    tss_full.entry.iomap_base = (unsigned short)sizeof(struct tss_entry);
    loaded_map = tss_full.iomap;
    __asm__ volatile("mov $0x28, %ax; ltr %ax");
}

void tss_set_esp0(unsigned int esp) {
    tss_full.entry.esp0 = esp;
}

void tss_set_esp_df(unsigned int esp) {
    tss_full.entry.esp_df = esp;
}

const unsigned char *tss_default_iomap(void) {
    return default_map;
}

void tss_load_iomap(const unsigned char *map) {
    if (loaded_map == map) return;
    map_copy(tss_full.iomap, map);
    loaded_map = map;
}

int tss_allow_ports(struct task *t, unsigned int port, unsigned int count) {
    if (!t) return -1;
    if (count == 0 || port + count > 0x10000) return -1;
    if (!t->iomap) {
        if (private_map_count >= MAX_PRIVATE_MAPS) return -1;
        unsigned char *m = private_maps[private_map_count++];
        map_copy(m, default_map);
        t->iomap = m;
    }
    unsigned char *m = (unsigned char *)t->iomap;
    for (unsigned int p = port; p < port + count; p++)
        m[p >> 3] &= (unsigned char)~(1u << (p & 7));
    loaded_map = 0;
    tss_load_iomap(t->iomap);
    return 0;
}
