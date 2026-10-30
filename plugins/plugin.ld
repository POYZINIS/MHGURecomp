/* Forge plugins (plugins/, `make plugins`): one flat image laid out as an NRO is in memory (tools/elf2nro.py): text
   from address 0 with its first 0x100 bytes left free (the NRO header, then MOD0), then the read-only data, then the
   data, each on a page of its own; bss last. */
SECTIONS
{
    . = 0;
    .text : {
        . = . + 0x100;
        *(.text .text.*)
        *(.plt)
    } =0
    . = ALIGN(0x1000);
    .rodata : { *(.rodata .rodata.*) }
    .dynsym : { *(.dynsym) }
    .dynstr : { *(.dynstr) }
    .hash : { *(.hash) }
    .rel.dyn : { *(.rel.dyn) }
    .rel.plt : { *(.rel.plt) }
    . = ALIGN(0x1000);
    .data : { *(.data.rel.ro .data.rel.ro.*) *(.data .data.*) }
    .init_array : { *(.init_array) }
    .dynamic : { *(.dynamic) }
    .got : { *(.got.plt) *(.got) }
    .bss : { *(.bss .bss.*) *(COMMON) }
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) *(.ARM.attributes) }
}
