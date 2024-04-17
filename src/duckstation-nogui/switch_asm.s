.text

.extern switch_exception_stack_top
.extern switch_exception_handler

#define EXCEPTION_STACK_SIZE 0x8000

.global __libnx_exception_entry
__libnx_exception_entry:
    adrp x2, switch_exception_stack_top
    add x2, x2, #:lo12:switch_exception_stack_top
    add sp, x2, EXCEPTION_STACK_SIZE

    sub sp, sp, #16*37

    // we only need to take care of the
    // of caller saved registers which aren't
    // in the exception frame
    stp x9, x10, [sp]
    stp x11, x12, [sp, #16*1]
    stp x13, x14, [sp, #16*2]
    stp x15, x16, [sp, #16*3]
    stp x17, x18, [sp, #16*4]

    // store all the vector registers
    // Unfortunately only the double part (lower 8-byte) of
    // v8-v15 is callee saved. So to keep the exact state we need
    // to store all registers (storing just the upper part
    // is more work than the whole 16 bytes)
    stp q0, q1, [sp, #16*5]
    stp q2, q3, [sp, #16*7]
    stp q4, q5, [sp, #16*9]
    stp q6, q7, [sp, #16*11]
    stp q8, q9, [sp, #16*13]
    stp q10, q11, [sp, #16*15]
    stp q12, q13, [sp, #16*17]
    stp q14, q15, [sp, #16*19]
    stp q16, q17, [sp, #16*21]
    stp q18, q19, [sp, #16*23]
    stp q20, q21, [sp, #16*25]
    stp q22, q23, [sp, #16*27]
    stp q24, q25, [sp, #16*29]
    stp q26, q27, [sp, #16*31]
    stp q28, q29, [sp, #16*33]
    stp q30, q31, [sp, #16*35]

    // setup parameters (x0=exception reason, x1=exception frame, x2=fp)
    mov x2, x29
    bl switch_exception_handler

    // load all vector registers
    ldp q0, q1, [sp, #16*5]
    ldp q2, q3, [sp, #16*7]
    ldp q4, q5, [sp, #16*9]
    ldp q6, q7, [sp, #16*11]
    ldp q8, q9, [sp, #16*13]
    ldp q10, q11, [sp, #16*15]
    ldp q12, q13, [sp, #16*17]
    ldp q14, q15, [sp, #16*19]
    ldp q16, q17, [sp, #16*21]
    ldp q18, q19, [sp, #16*23]
    ldp q20, q21, [sp, #16*25]
    ldp q22, q23, [sp, #16*27]
    ldp q24, q25, [sp, #16*29]
    ldp q26, q27, [sp, #16*31]
    ldp q28, q29, [sp, #16*33]
    ldp q30, q31, [sp, #16*35]

    ldp x9, x10, [sp]
    ldp x11, x12, [sp, #16*1]
    ldp x13, x14, [sp, #16*2]
    ldp x15, x16, [sp, #16*3]
    ldp x17, x18, [sp, #16*4]

    // hehe we don't need to restore the stack pointer

    // let the kernel restore everything else
    mov x0, 0
    bl svcReturnFromException
