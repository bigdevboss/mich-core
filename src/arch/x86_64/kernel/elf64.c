#include "elf64.h"
#include "vm64.h"

#define ET_EXEC 2
#define EM_X86_64 62
#define PT_LOAD 1
#define PF_X 1
#define PF_W 2
#define ELF64_PH_MAX 32

struct elf64_header {
    u8 ident[16];
    u16 type;
    u16 machine;
    u32 version;
    u64 entry;
    u64 phoff;
    u64 shoff;
    u32 flags;
    u16 ehsize;
    u16 phentsize;
    u16 phnum;
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
};

struct elf64_phdr {
    u32 type;
    u32 flags;
    u64 offset;
    u64 vaddr;
    u64 paddr;
    u64 filesz;
    u64 memsz;
    u64 align;
};

static const struct elf64_phdr *program_header(const u8 *image,
                                                const struct elf64_header *header,
                                                u32 index) {
    return (const struct elf64_phdr *)(const void *)
        (image + header->phoff + (u64)index * header->phentsize);
}

static int range_overlaps(u64 a_start, u64 a_end, u64 b_start, u64 b_end) {
    return a_start < b_end && b_start < a_end;
}

int elf64_load(u32 space, const u8 *image, u32 size, vaddr_t *entry_out) {
    if (!image || !entry_out || size < sizeof(struct elf64_header)) return -1;
    const struct elf64_header *header =
        (const struct elf64_header *)(const void *)image;
    if (header->ident[0] != 0x7F || header->ident[1] != 'E' ||
        header->ident[2] != 'L' || header->ident[3] != 'F' ||
        header->ident[4] != 2 || header->ident[5] != 1 ||
        header->ident[6] != 1)
        return -2;
    if (header->type != ET_EXEC || header->machine != EM_X86_64 ||
        header->version != 1 || header->ehsize < sizeof(*header))
        return -3;
    if (!header->phnum || header->phnum > ELF64_PH_MAX ||
        header->phentsize < sizeof(struct elf64_phdr))
        return -4;
    u64 phbytes = (u64)header->phnum * header->phentsize;
    if (header->phoff > size || phbytes > (u64)size - header->phoff)
        return -5;

    u32 pages = 0;
    int entry_valid = 0;
    for (u32 i = 0; i < header->phnum; i++) {
        const struct elf64_phdr *segment = program_header(image, header, i);
        if (segment->type != PT_LOAD || !segment->memsz) continue;
        if (segment->filesz > segment->memsz || segment->offset > size ||
            segment->filesz > (u64)size - segment->offset)
            return -6;
        if ((segment->vaddr & 0xFFF) || (segment->offset & 0xFFF) ||
            segment->vaddr < VM64_PROGRAM_BASE ||
            segment->vaddr >= VM64_PROGRAM_LIMIT ||
            segment->memsz > VM64_PROGRAM_LIMIT - segment->vaddr)
            return -7;
        if (segment->align > 1 &&
            ((segment->align & (segment->align - 1)) || segment->align > 4096))
            return -8;
        if ((segment->flags & (PF_W | PF_X)) == (PF_W | PF_X)) return -9;
        u64 page_end = (segment->vaddr + segment->memsz + 0xFFF) & ~0xFFFULL;
        if (segment->vaddr < VM64_STACK_TOP &&
            VM64_STACK_TOP - 4096 < page_end)
            return -10;
        pages += (u32)((page_end - segment->vaddr) >> 12);
        if ((segment->flags & PF_X) && header->entry >= segment->vaddr &&
            header->entry < segment->vaddr + segment->memsz)
            entry_valid = 1;
        for (u32 j = 0; j < i; j++) {
            const struct elf64_phdr *other = program_header(image, header, j);
            if (other->type != PT_LOAD || !other->memsz) continue;
            u64 other_end = (other->vaddr + other->memsz + 0xFFF) & ~0xFFFULL;
            if (range_overlaps(segment->vaddr, page_end, other->vaddr, other_end))
                return -10;
        }
    }
    if (!entry_valid || !pages || pages > vm64_available_pages()) return -11;

    for (u32 i = 0; i < header->phnum; i++) {
        const struct elf64_phdr *segment = program_header(image, header, i);
        if (segment->type != PT_LOAD || !segment->memsz) continue;
        u64 page_end = (segment->vaddr + segment->memsz + 0xFFF) & ~0xFFFULL;
        for (u64 virt = segment->vaddr; virt < page_end; virt += 4096) {
            paddr_t phys = vm64_alloc_page();
            if (!phys) return -12;
            u8 *destination = (u8 *)(uptr_t)phys;
            u64 offset = virt - segment->vaddr;
            u64 count = 0;
            if (offset < segment->filesz) {
                count = segment->filesz - offset;
                if (count > 4096) count = 4096;
                const u8 *source = image + segment->offset + offset;
                for (u64 byte = 0; byte < count; byte++) destination[byte] = source[byte];
            }
            for (u64 byte = count; byte < 4096; byte++) destination[byte] = 0;
            if (vm64_map(space, virt, phys, (segment->flags & PF_W) != 0,
                         (segment->flags & PF_X) != 0) != 0) {
                vm64_free_page(phys);
                return -13;
            }
        }
    }
    *entry_out = header->entry;
    return 0;
}
