#!/usr/bin/env python3
import struct
import sys

BLOCK = 512
BOOT_BLOCKS = 1112
BLOB_LBA = 32
BLOB_MAGIC = 0x424F4C42
BASE = 0x100000
MAX_MODULES = 8


def kernel_info(path):
    with open(path, "rb") as file:
        image = file.read()
    if len(image) < 64 or image[:4] != b"\x7fELF" or image[4] != 2 or image[5] != 1:
        raise SystemExit("kernel is not ELF64 little-endian")
    entry = struct.unpack_from("<Q", image, 24)[0]
    phoff = struct.unpack_from("<Q", image, 32)[0]
    phentsize = struct.unpack_from("<H", image, 54)[0]
    phnum = struct.unpack_from("<H", image, 56)[0]
    if phentsize < 56 or phoff + phentsize * phnum > len(image):
        raise SystemExit("invalid ELF64 program headers")
    end = BASE
    bss_off = 0
    bss_len = 0
    for index in range(phnum):
        position = phoff + index * phentsize
        p_type = struct.unpack_from("<I", image, position)[0]
        if p_type != 1:
            continue
        vaddr = struct.unpack_from("<Q", image, position + 16)[0]
        filesz = struct.unpack_from("<Q", image, position + 32)[0]
        memsz = struct.unpack_from("<Q", image, position + 40)[0]
        if memsz < filesz or vaddr + memsz < vaddr:
            raise SystemExit("invalid ELF64 load segment")
        end = max(end, vaddr + memsz)
        if memsz > filesz:
            bss_off = vaddr + filesz - BASE
            bss_len = memsz - filesz
    if entry < BASE or entry >= end:
        raise SystemExit("invalid ELF64 entry")
    return entry - BASE, bss_off, bss_len, (end + 0xFFF) & ~0xFFF


def read_modules(arguments):
    modules = []
    for specification in arguments:
        if "=" not in specification:
            raise SystemExit("bad module specification: " + specification)
        identity, path = specification.split("=", 1)
        if ":" in identity:
            name, capability_text = identity.rsplit(":", 1)
            capabilities = int(capability_text, 0)
        else:
            name = identity
            capabilities = 0
        encoded = name.encode("ascii")
        if not encoded or len(encoded) > 15 or capabilities < 0 or capabilities > 0xFFFFFFFF:
            raise SystemExit("invalid module identity: " + identity)
        with open(path, "rb") as file:
            data = file.read()
        if not data:
            raise SystemExit("empty module: " + path)
        modules.append((encoded, data, capabilities))
    if len(modules) > MAX_MODULES:
        raise SystemExit("too many modules")
    return modules


def main():
    args = sys.argv[1:]
    kernel_elf = "bin/x86_64/mich-kernel.elf"
    kernel_flat = "bin/x86_64/mich-kernel.bin"
    while args and args[0].startswith("--"):
        if len(args) < 2 or args[0] not in ("--kernel", "--kernel-flat"):
            raise SystemExit("bad kernel option")
        if args[0] == "--kernel":
            kernel_elf = args[1]
        else:
            kernel_flat = args[1]
        args = args[2:]
    if not args:
        raise SystemExit("usage: mkboot64.py [--kernel ELF] [--kernel-flat BIN] OUTPUT [NAME:CAPS=MODULE ...]")
    output = args[0]
    modules = read_modules(args[1:])
    entry, bss_off, bss_len, module_base = kernel_info(kernel_elf)
    with open("bin/x86_64/bdb1.bin", "rb") as file:
        stage1 = file.read()
    with open("bin/x86_64/bdb2.bin", "rb") as file:
        stage2 = file.read()
    with open(kernel_flat, "rb") as file:
        kernel = file.read()
    if len(stage1) != BLOCK or stage1[510:512] != b"\x55\xaa":
        raise SystemExit("invalid stage1")
    if len(stage2) > 16 * BLOCK:
        raise SystemExit("stage2 too large")

    kernel_offset = 0x1000
    cursor = (kernel_offset + len(kernel) + BLOCK - 1) & ~(BLOCK - 1)
    records = []
    for name, data, capabilities in modules:
        records.append((cursor, len(data), name, capabilities))
        cursor = (cursor + len(data) + BLOCK - 1) & ~(BLOCK - 1)
    blob_length = cursor
    blob = bytearray(blob_length)
    blob[kernel_offset:kernel_offset + len(kernel)] = kernel
    for (_, data, _), (offset, size, _, _) in zip(modules, records):
        blob[offset:offset + size] = data

    struct.pack_into("<I", blob, 0, BLOB_MAGIC)
    struct.pack_into("<I", blob, 4, blob_length // BLOCK)
    struct.pack_into("<I", blob, 8, kernel_offset)
    struct.pack_into("<I", blob, 12, len(kernel))
    struct.pack_into("<I", blob, 16, entry)
    struct.pack_into("<I", blob, 20, len(records))
    for index, (offset, size, name, capabilities) in enumerate(records):
        position = 24 + index * 24
        struct.pack_into("<II", blob, position, offset, size)
        blob[position + 8:position + 8 + len(name)] = name
        struct.pack_into("<I", blob, 236 + index * 4, capabilities)
    struct.pack_into("<I", blob, 224, module_base)
    struct.pack_into("<I", blob, 228, bss_off)
    struct.pack_into("<I", blob, 232, bss_len)

    if BLOB_LBA * BLOCK + blob_length > BOOT_BLOCKS * BLOCK:
        raise SystemExit("image too large")
    disk = bytearray(BOOT_BLOCKS * BLOCK)
    disk[:BLOCK] = stage1
    disk[BLOCK:BLOCK + len(stage2)] = stage2
    start = BLOB_LBA * BLOCK
    disk[start:start + blob_length] = blob
    with open(output, "wb") as file:
        file.write(disk)
    print("mkboot64: stage2 %dB, blob %dB, entry +0x%x, modules %d" %
          (len(stage2), blob_length, entry, len(records)))


if __name__ == "__main__":
    main()
