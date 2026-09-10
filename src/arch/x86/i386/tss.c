#include "tss.h"
#include "task.h"
#include "scheduler.h"
#include "arch_task.h"

#define MAX_PRIVATE_MAPS MAX_TASKS

struct tss_full tss_full;
struct tss_entry df_tss;

static unsigned char df_stack[4096] __attribute__((aligned(4096)));
static unsigned char init_stack[4096] __attribute__((aligned(4096)));

extern void df_task_entry(void);
extern unsigned int paging_kernel_dir_phys(void);

static unsigned char default_map[TSS_IOMAP_BYTES + 1];
static unsigned char private_maps[MAX_PRIVATE_MAPS][TSS_IOMAP_BYTES + 1];
static unsigned char map_used[MAX_PRIVATE_MAPS];
static const unsigned char *loaded_map = 0;

extern void serial_write(const char *str);

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
    tss_full.entry.esp0 = (unsigned int)(init_stack + 4096);
    tss_full.entry.iomap_base = (unsigned short)sizeof(struct tss_entry);
    loaded_map = tss_full.iomap;

    unsigned char *q = (unsigned char *)&df_tss;
    for (unsigned int i = 0; i < sizeof(struct tss_entry); i++) q[i] = 0;
    df_tss.eip = (unsigned int)df_task_entry;
    df_tss.eflags = 0x2;
    df_tss.cs = 0x08;
    df_tss.ss = 0x10;
    df_tss.ds = 0x10;
    df_tss.es = 0x10;
    df_tss.fs = 0x10;
    df_tss.gs = 0x10;
    df_tss.esp = (unsigned int)(df_stack + 4096);
    df_tss.cr3 = paging_kernel_dir_phys();
    df_tss.iomap_base = 0xFFFF;

    __asm__ volatile("mov $0x28, %ax; ltr %ax");
}

void tss_set_esp0(unsigned int esp) {
    tss_full.entry.esp0 = esp;
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
    if (count == 0 || port >= 0x10000 || count > 0x10000 - port) return -1;
    if (!t->iomap || t->iomap == default_map) {
        int slot = -1;
        for (int i = 0; i < MAX_PRIVATE_MAPS; i++) {
            if (!map_used[i]) { slot = i; break; }
        }
        if (slot < 0) return -1;
        map_used[slot] = 1;
        unsigned char *m = private_maps[slot];
        map_copy(m, default_map);
        t->iomap = m;
    }
    unsigned char *m = (unsigned char *)t->iomap;
    for (unsigned int p = port; p < port + count; p++)
        m[p >> 3] &= (unsigned char)~(1u << (p & 7));
    if (t == &task_pool[scheduler_current()]) {
        loaded_map = 0;
        tss_load_iomap(t->iomap);
    }
    return 0;
}

void tss_release_iomap(struct task *t) {
    if (!t || !t->iomap || t->iomap == default_map) return;
    for (int i = 0; i < MAX_PRIVATE_MAPS; i++) {
        if (t->iomap == private_maps[i]) {
            map_used[i] = 0;
            break;
        }
    }
    t->iomap = 0;
    if (loaded_map && loaded_map != (const unsigned char *)tss_full.iomap && loaded_map != default_map) {
        for (int i = 0; i < MAX_PRIVATE_MAPS; i++) {
            if (loaded_map == private_maps[i] && !map_used[i]) {
                loaded_map = 0;
                break;
            }
        }
    }
}

void arch_task_init(void) {
    tss_init();
}

void arch_task_activate(struct task *task) {
    if (task->kernel_stack)
        tss_set_esp0((unsigned int)(uptr_t)task->kernel_stack);
    tss_load_iomap(task->iomap ? task->iomap : tss_default_iomap());
}

int arch_task_allow_io(struct task *task, u32 port, u32 count) {
    return tss_allow_ports(task, port, count);
}

void arch_task_release(struct task *task) {
    tss_release_iomap(task);
}
