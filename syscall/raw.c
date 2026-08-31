/* raw.c -- crossing the barrier by hand, on four platforms.
**
** THE POINT OF THIS FILE: there is no address in it.
**
** A function call names a place to go. A system call does not. You put a
** number in a register, execute one instruction, and the HARDWARE decides
** where control lands -- in a vector table the kernel installed at boot, in
** memory this process cannot write. That is the whole barrier. Not a lock on
** a door: no door, and no handle to hold.
**
** Everything below is the same three lines four times, because the CONVENTION
** differs per platform and the IDEA does not. Read the #if blocks as a survey
** of arbitrary choices: which register holds the number, which instruction
** raises the exception, which numbers mean "write".
**
**   cc -std=c11 -O0 -o raw raw.c && ./raw
*/
#include <stddef.h>

/* ------------------------------------------------------------------ arm64 */
#if defined(__aarch64__)

/* The number goes in a register, the arguments in x0..x5, and one instruction
** traps. On Linux that instruction is `svc #0` and the number lives in x8; on
** macOS it is `svc #0x80` and the number lives in x16. Nothing else differs.
**
** Note what is NOT here: a return address. `svc` parks it in ELR_EL1, which
** is a system register at a privilege level this code cannot touch -- the same
** trick as `LR` for an ordinary call, one level up, and out of reach.
*/
static long sys3(long n, long a, long b, long c) {
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
# if defined(__APPLE__)
    register long x16 __asm__("x16") = n;
    __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x16) : "memory", "cc");
# else
    register long x8 __asm__("x8") = n;
    __asm__ volatile("svc #0"    : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8)  : "memory", "cc");
# endif
    return x0;
}

/* ----------------------------------------------------------------- x86-64 */
#elif defined(__x86_64__)

/* Same idea, different furniture: the number goes in rax, arguments in
** rdi/rsi/rdx, and `syscall` traps. macOS ORs in a class bit (0x2000000 marks
** a BSD call) because Darwin's kernel serves more than one personality.
**
** The return address goes to rcx and the flags to r11, chosen by the CPU, not
** by this code.
*/
static long sys3(long n, long a, long b, long c) {
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory", "cc");
    return ret;
}
#else
# error "This demo is arm64 or x86-64 only -- deliberately, since the point is the ISA convention."
#endif

/* ------------------------------------------------- the numbers themselves */
/* These are the arbitrary part. Linux's numbers are a STABLE ABI: number 1 on
** x86-64 will mean write forever, which is why a program built in 2010 still
** runs. Darwin's are NOT a supported interface -- Apple's boundary is
** libSystem, not the kernel, and it reserves the right to renumber. That
** difference is the homework's whole problem; see README.
*/
#if defined(__APPLE__)
# if defined(__x86_64__)
#  define CLASS 0x2000000L      /* BSD personality */
# else
#  define CLASS 0L
# endif
# define SYS_WRITE  (CLASS + 4)
# define SYS_EXIT   (CLASS + 1)
# define SYS_GETPID (CLASS + 20)
#elif defined(__aarch64__)
# define SYS_WRITE  64
# define SYS_EXIT   93
# define SYS_GETPID 172
#else
# define SYS_WRITE  1
# define SYS_EXIT   60
# define SYS_GETPID 39
#endif

long raw_write(int fd, const void *buf, unsigned long n) {
    return sys3(SYS_WRITE, fd, (long)buf, (long)n);
}
long raw_getpid(void) { return sys3(SYS_GETPID, 0, 0, 0); }
void raw_exit(int code) { sys3(SYS_EXIT, code, 0, 0); __builtin_unreachable(); }

#ifdef RAW_MAIN
int main(void) {
    static const char msg[] = "hello from the other side of the barrier\n";
    raw_write(1, msg, sizeof msg - 1);
    raw_exit(0);
}
#endif
