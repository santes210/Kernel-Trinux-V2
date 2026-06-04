#ifndef SHELL_TASM_H
#define SHELL_TASM_H

/* tasm — Trinux Assembler.
 *
 * A minimal x86 assembler built into the shell that can assemble simple
 * programs into ELF32 executables stored in the VFS.
 *
 * Usage from the shell:
 *   edit /home/user/myapp.asm     (write your code)
 *   asm /home/user/myapp.asm      (assemble → creates /home/user/myapp)
 *   exec /home/user/myapp         (run it)
 *
 * Supported syntax (Intel-style):
 *   mov eax, 1           ; SYS_WRITE
 *   mov ebx, 1           ; fd
 *   mov ecx, msg         ; buffer
 *   mov edx, 13          ; length
 *   int 0x80             ; syscall
 *   mov eax, 0           ; SYS_EXIT
 *   mov ebx, 0           ; status
 *   int 0x80
 *   ret
 *   msg: db "Hello World!", 10
 *
 * Supported instructions:
 *   mov reg, imm / mov reg, reg / mov reg, label
 *   add, sub, and, or, xor reg, imm/reg
 *   inc, dec reg
 *   push, pop reg
 *   int imm
 *   call label / jmp label / je/jne/jz/jnz label
 *   cmp reg, imm/reg
 *   ret / nop / hlt
 *   db "string", byte, ...     (data)
 *   label:                     (define label)
 *   ; comment
 */

int tasm_assemble(const char *src_path, const char *out_path);

#endif /* SHELL_TASM_H */
