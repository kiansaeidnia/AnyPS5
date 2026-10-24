.cfi_sections .eh_frame
.text
.globl UnwindAllocationFrame
UnwindAllocationFrame:
    .cfi_startproc
    sub $8, %rsp
    .cfi_def_cfa_offset 16
    mov %rdi, %rax
    mov %rsi, %rdi
    call *%rax
    add $8, %rsp
    .cfi_def_cfa_offset 8
    ret
    .cfi_endproc
