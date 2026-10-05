.text

.globl context_switch
.type context_switch, @function

# ------------------------------------------------------------
# void context_switch(struct context *old,
#                     const struct context *new_ctx)
#
# SysV AMD64:
#
#   RDI = old
#   RSI = new
#
# struct context:
#
#   +0   rsp
#   +8   rip
#   +16  rbp
#   +24  rbx
#   +32  r12
#   +40  r13
#   +48  r14
#   +56  r15
# ------------------------------------------------------------

context_switch:

    # Save current stack pointer
    movq %rsp, 0(%rdi)

    # Save return address
    movq (%rsp), %rax
    movq %rax, 8(%rdi)

    # Save callee-saved registers
    movq %rbp, 16(%rdi)
    movq %rbx, 24(%rdi)
    movq %r12, 32(%rdi)
    movq %r13, 40(%rdi)
    movq %r14, 48(%rdi)
    movq %r15, 56(%rdi)


    # --------------------------------------------------------
    # Restore new context
    # --------------------------------------------------------

    movq 0(%rsi), %rsp

    movq 16(%rsi), %rbp
    movq 24(%rsi), %rbx
    movq 32(%rsi), %r12
    movq 40(%rsi), %r13
    movq 48(%rsi), %r14
    movq 56(%rsi), %r15

    # New instruction pointer
    movq 8(%rsi), %rax

    # context_switch() finishes with ret.
    # Therefore put RIP on the new stack.
    movq %rax, (%rsp)

    ret


.size context_switch, .-context_switch

.section .note.GNU-stack,"",@progbits