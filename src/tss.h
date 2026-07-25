#ifndef TSS_H
#define TSS_H

#define TSS_IOMAP_BYTES 8192

struct tss_entry {
    unsigned int prev_tss;
    unsigned int esp0;
    unsigned int ss0;
    unsigned int esp1;
    unsigned int ss1;
    unsigned int esp2;
    unsigned int ss2;
    unsigned int cr3;
    unsigned int eip;
    unsigned int eflags;
    unsigned int eax;
    unsigned int ecx;
    unsigned int edx;
    unsigned int ebx;
    unsigned int esp;
    unsigned int ebp;
    unsigned int esi;
    unsigned int edi;
    unsigned int es;
    unsigned int cs;
    unsigned int ss;
    unsigned int ds;
    unsigned int fs;
    unsigned int gs;
    unsigned int ldt;
    unsigned short trap;
    unsigned short iomap_base;
    unsigned int esp_df;
} __attribute__((packed));

struct tss_full {
    struct tss_entry entry;
    unsigned char iomap[TSS_IOMAP_BYTES + 1];
} __attribute__((packed));

struct task;

extern struct tss_full tss_full;

void tss_init(void);
void tss_set_esp0(unsigned int esp);
void tss_set_esp_df(unsigned int esp);
const unsigned char *tss_default_iomap(void);
void tss_load_iomap(const unsigned char *map);
int tss_allow_ports(struct task *t, unsigned int port, unsigned int count);

#endif
