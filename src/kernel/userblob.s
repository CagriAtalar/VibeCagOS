/* Embeds the separately built user binary (utest.bin) into the kernel image. */
.section .rodata
.balign 4
.global utest_bin_start
.global utest_bin_end
utest_bin_start:
    .incbin "utest.bin"
utest_bin_end:
