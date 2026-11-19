.section .rdata,"dr"
.balign 16
.globl np_embed_hook64_begin
.globl np_embed_hook64_end
np_embed_hook64_begin:
    .incbin "dist/NextPerfHook.dll"
np_embed_hook64_end:

.balign 16
.globl np_embed_hook32_begin
.globl np_embed_hook32_end
np_embed_hook32_begin:
    .incbin "dist/NextPerfHook32.dll"
np_embed_hook32_end:
