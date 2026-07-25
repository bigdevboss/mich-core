#!/usr/bin/env python3
import struct
import sys

BLOCK = 512
BOOT_BLOCKS = 256
STAGE2_BYTES = 16 * 512
BLOB_LBA = 32
BLOB_MAGIC = 0x424F4C42

def elf_kinfo(path):
    with open(path, "rb") as f:
        hdr = f.read(52)
        f.seek(0)
        phoff = struct.unpack_from("<I", hdr, 28)[0]
        entry = struct.unpack_from("<I", hdr, 24)[0]
        phentsize = struct.unpack_from("<H", hdr, 42)[0]
        phnum = struct.unpack_from("<H", hdr, 44)[0]
        f.seek(phoff)
        ph = f.read(phentsize * phnum)
    bss_off = 0
    bss_len = 0
    kend = 0
    for i in range(phnum):
        base = i * phentsize
        p_type = struct.unpack_from("<I", ph, base + 0)[0]
        if p_type != 1:
            continue
        vaddr = struct.unpack_from("<I", ph, base + 8)[0]
        filesz = struct.unpack_from("<I", ph, base + 16)[0]
        memsz = struct.unpack_from("<I", ph, base + 20)[0]
        kend = max(kend, vaddr + memsz)
        if memsz > filesz:
            bss_off = vaddr + filesz - 0x100000
            bss_len = memsz - filesz
    mod_base = (kend + 0xFFF) & ~0xFFF
    return entry, bss_off, bss_len, mod_base

def main():
    out = sys.argv[1]
    entry, bss_off, bss_len, mod_base = elf_kinfo("mich-kernel.bin")
    with open("bdb1.bin", "rb") as f:
        stage1 = f.read()
    assert len(stage1) == 512 and stage1[510:512] == b"\x55\xaa"
    with open("bdb2.bin", "rb") as f:
        stage2 = f.read()
    assert len(stage2) <= STAGE2_BYTES
    with open("kflat.bin", "rb") as f:
        kflat = f.read()
    koff = 0x1000
    blob_len = (koff + len(kflat) + 511) & ~511
    blob = bytearray(blob_len)
    blob[koff:koff + len(kflat)] = kflat
    hdr = bytearray(512)
    struct.pack_into("<I", hdr, 0, BLOB_MAGIC)
    struct.pack_into("<I", hdr, 4, blob_len // 512)
    struct.pack_into("<I", hdr, 8, koff)
    struct.pack_into("<I", hdr, 12, len(kflat))
    struct.pack_into("<I", hdr, 16, entry - 0x100000)
    struct.pack_into("<I", hdr, 20, 0)
    struct.pack_into("<I", hdr, 224, mod_base)
    struct.pack_into("<I", hdr, 228, bss_off)
    struct.pack_into("<I", hdr, 232, bss_len)
    blob[0:512] = hdr
    assert BLOB_LBA * 512 + blob_len <= BOOT_BLOCKS * 512
    zone = bytearray(BOOT_BLOCKS * 512)
    zone[0:512] = stage1
    zone[512:512 + len(stage2)] = stage2
    zone[BLOB_LBA * 512:BLOB_LBA * 512 + blob_len] = blob
    with open(out, "wb") as f:
        f.write(zone)
    print("mkboot: bigdevboot zone: stage1 512B, stage2 %dB, blob %dB (kentry +0x%x, bss %dB, mods 0)"
          % (len(stage2), blob_len, entry - 0x100000, bss_len))

if __name__ == "__main__":
    main()
