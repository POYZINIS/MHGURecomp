"""elf2nro.py <plugin.elf> <plugin.nro>: a Forge plugin linked with plugins/plugin.ld (an AArch32 shared object laid
out flat from address 0: text, read-only data, data, bss) as the NRO the port loads (runtime/loader.c,
loader_load_nro): the image as it is in memory, with the NRO header in the first 0x80 bytes of its text (left free by
the linker script) and MOD0 after it, pointing at .dynamic."""
import struct
import sys

PT_LOAD, PT_DYNAMIC, PF_X, PF_W = 1, 2, 1, 2
MOD0_AT = 0x80


def main():
    src, dst = sys.argv[1], sys.argv[2]
    elf = open(src, 'rb').read()
    if elf[:4] != b'\x7fELF' or elf[4] != 1 or elf[5] != 1 or struct.unpack_from('<H', elf, 18)[0] != 40:
        sys.exit(f'{src}: not a little-endian 32-bit ARM ELF')
    phoff, = struct.unpack_from('<I', elf, 28)
    phentsize, phnum = struct.unpack_from('<HH', elf, 42)
    loads, dynamic = [], None
    for i in range(phnum):
        p_type, off, vaddr, _, filesz, memsz, flags, _ = struct.unpack_from('<8I', elf, phoff + i * phentsize)
        if p_type == PT_LOAD and memsz:
            loads.append((vaddr, off, filesz, memsz, flags))
        elif p_type == PT_DYNAMIC:
            dynamic = vaddr
    loads.sort()
    if len(loads) != 3 or [l[4] & (PF_X | PF_W) for l in loads] != [PF_X, 0, PF_W] or loads[0][0] != 0 or dynamic is None:
        sys.exit(f'{src}: expected text at 0, then read-only data, then data (plugins/plugin.ld), and a .dynamic')
    (text, _, _, _, _), (ro, _, _, _, _), (data, _, data_file, data_mem, _) = loads
    if ro % 0x1000 or data % 0x1000:
        sys.exit(f'{src}: segments not page-aligned')
    img = bytearray(data + data_file)
    for vaddr, off, filesz, _, _ in loads:
        img[vaddr:vaddr + filesz] = elf[off:off + filesz]
    if any(img[:0x100]):
        sys.exit(f'{src}: the first 0x100 bytes of text are not free for the NRO header (plugins/plugin.ld)')
    bss = data_mem - data_file
    struct.pack_into('<II', img, 0, 0, MOD0_AT)  # (no start code: a plugin is not run from its first word)
    img[0x10:0x14] = b'NRO0'
    struct.pack_into('<III', img, 0x14, 0, len(img), 0)  # version, size, flags
    struct.pack_into('<6I', img, 0x20, text, ro - text, ro, data - ro, data, data_file)
    struct.pack_into('<I', img, 0x38, bss)
    img[MOD0_AT:MOD0_AT + 4] = b'MOD0'
    struct.pack_into('<3i', img, MOD0_AT + 4, dynamic - MOD0_AT, data + data_file - MOD0_AT, data + data_mem - MOD0_AT)
    open(dst, 'wb').write(img)
    print(f'{dst}: text {ro - text:#x}, rodata {data - ro:#x}, data {data_file:#x}, bss {bss:#x}')


if __name__ == '__main__':
    main()
