/*
 * void task_switch(uint32_t *old_esp, uint32_t new_esp);
 *
 * Save the current task's callee-saved registers and EFLAGS on its
 * stack, store the resulting ESP into *old_esp, then load the
 * incoming task's stack into ESP and restore its registers/EFLAGS.
 *
 * After the final `ret`:
 *   - For a previously-suspended task, control returns to the
 *     instruction following its earlier task_switch() call.
 *   - For a brand-new task, control jumps straight to the entry
 *     function we synthesized into the initial stack frame.
 *
 * cdecl: arg1 at [esp+4], arg2 at [esp+8]. After the 5 pushes below,
 * arg1 is at [esp+24] and arg2 at [esp+28].
 */

.code32
.section .text

.global _task_switch
_task_switch:
    pushfl
    cli                         /* atomicity during the actual swap */
    push    %ebp
    push    %ebx
    push    %esi
    push    %edi

    movl    24(%esp), %eax      /* old_esp */
    movl    %esp, (%eax)        /* *old_esp = current ESP */

    movl    28(%esp), %esp      /* esp = new_esp */

    pop     %edi
    pop     %esi
    pop     %ebx
    pop     %ebp
    popfl                       /* restores IF for the incoming task */
    ret

/*
 * Trampoline used by task_fork(): identical to the tail of
 * isr_common_stub. The forked child's kernel stack was synthesized so
 * that task_switch's `ret` jumps here, and the iret frame at the top
 * of that stack matches what the parent's syscall entry pushed (with
 * EAX zeroed so fork() returns 0 in the child).
 */
.global _fork_child_return
_fork_child_return:
    pop     %eax                /* saved DS                          */
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs

    popa                        /* restores all GP regs (eax = 0)    */
    add     $8, %esp            /* skip int_no + err_code            */
    iret                        /* back to ring 3 at parent's EIP    */
