/* kernel/longmode_detect.c — CAMBIO #11: detección de Long Mode via CPUID.
 * Separado aquí para no contaminar el wildcard de cpu/*.c con un stub. */
#include "../lib/types.h"

int longmode_check(void)
{
    uint32_t eax, edx;
    __asm__ volatile("cpuid" : "=a"(eax) : "a"(0x80000000u) : "ebx","ecx","edx");
    if (eax < 0x80000001u) return 0;
    __asm__ volatile("cpuid" : "=a"(eax),"=d"(edx) : "a"(0x80000001u) : "ebx","ecx");
    return (edx & (1u << 29)) ? 1 : 0;
}
