/*
 * memmap.c -- where does a process keep its stuff?
 *
 * Prints the address of:
 *   - code   : main() and greet()            (text segment, executable)
 *   - rodata : the literal "Hello, "         (read-only data)
 *   - data   : a writable global "Hello, "   (initialized data)
 *   - bss    : an uninitialized global       (zero-filled data)
 *   - heap   : malloc'd "Hello, " + who      (grows up, toward the stack)
 *   - stack  : greet()'s parameter and locals(grows down, toward the heap)
 *
 * Build:  make          Run:  ./memmap [name]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ---- statically allocated storage ------------------------------------ */

/* .data  : initialized and writable, so the bytes live in the image and get
 *          copied into a writable page at exec time. */
char global_greeting[] = "Hello, ";

/* .rodata: the literal itself is read-only; global_literal is a .data
 *          pointer that points at it. Two different addresses! */
const char *global_literal = "Hello, ";

/* .bss   : no initializer, so nothing is stored in the executable -- the
 *          kernel just hands us zeroed pages. */
static char bss_scratch[4096];

/* ---- a little address-map bookkeeping -------------------------------- */

#define MAX_MARKS 32

#if defined(__x86_64__)
#  define ARCH "x86_64"
#elif defined(__aarch64__)
#  define ARCH "arm64"
#else
#  define ARCH "unknown-arch"
#endif

struct mark {
    const char *segment;   /* text / rodata / data / bss / heap / stack / env */
    const char *what;      /* human description */
    const void *addr;
};

static struct mark marks[MAX_MARKS];
static int nmarks = 0;

static void note(const char *segment, const char *what, const void *addr)
{
    if (nmarks < MAX_MARKS) {
        marks[nmarks].segment = segment;
        marks[nmarks].what    = what;
        marks[nmarks].addr    = addr;
        nmarks++;
    }
}

/* Descending: highest address first. */
static int by_addr_desc(const void *a, const void *b)
{
    const void *x = ((const struct mark *)a)->addr;
    const void *y = ((const struct mark *)b)->addr;
    /* compare as integers; C only defines < > for pointers into the same
     * object, and these are deliberately in different objects. */
    unsigned long xi = (unsigned long)x, yi = (unsigned long)y;
    return (xi < yi) - (xi > yi);
}

static void human(unsigned long bytes, char *out, size_t n)
{
    static const char *unit[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    int u = 0;
    double v = (double)bytes;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; u++; }
    snprintf(out, n, "%.1f %s", v, unit[u]);
}

/* ---- the function whose frame we inspect ----------------------------- */

/* What greet() learned about its own stack frame, kept so main() can report
 * it after greet() has returned and the frame itself is gone. */
static struct {
    void  *ret_value;   /* the saved PC, as the compiler reports it */
    void **fp;          /* greet's frame pointer */
    void **saved_fp;    /* the slot holding the caller's frame pointer */
    void **ret_slot;    /* the slot holding the saved PC */
    void  *ret_live;    /* what that slot held WHILE greet was still running */
    void  *fp_live;     /* likewise for the saved frame pointer */
} greet_frame;

/*
 * greet() builds "Hello, " + who on the heap and returns it.
 * The caller owns the result and must free() it.
 *
 * Along the way it records the addresses of its own parameter and locals,
 * which live in greet's *stack frame* -- valid only until greet returns.
 */
char *greet(const char *who)
{
    size_t need = strlen(global_greeting) + strlen(who) + 1;  /* +1 for '\0' */
    char *message = malloc(need);                             /* heap block */
    if (message == NULL) {
        perror("malloc");
        exit(EXIT_FAILURE);
    }
    strcpy(message, global_greeting);
    strcat(message, who);

    note("stack",  "greet: parameter 'who' (the pointer itself)", &who);
    note("stack",  "greet: local 'need'",                         &need);
    note("stack",  "greet: local 'message' (the pointer itself)", &message);
    note("heap",   "greet: malloc'd \"Hello, \" + who",           message);

    /* The saved program counter: where greet() resumes when it returns.
     *
     * Two ways to get at it. The compiler will tell us the value:      */
    greet_frame.ret_value = __builtin_return_address(0);

    /* ...and we can find the stack slot it is sitting in by hand. Both the
     * x86-64 SysV ABI and the AArch64 AAPCS put a two-word frame record at
     * the frame pointer: [0] is the caller's saved frame pointer and [1] is
     * the return address. (True at -O0, where frame pointers are kept.) */
    greet_frame.fp        = (void **)__builtin_frame_address(0);
    greet_frame.saved_fp  = &greet_frame.fp[0];
    greet_frame.ret_slot  = &greet_frame.fp[1];

    /* Read the slots now, while this frame is still live. Once greet returns
     * these addresses still exist, but they no longer belong to us. */
    greet_frame.fp_live   = *greet_frame.saved_fp;
    greet_frame.ret_live  = *greet_frame.ret_slot;

    note("stack",  "greet: slot holding the caller's saved frame pointer",
         greet_frame.saved_fp);
    note("stack",  "greet: slot holding the saved PC (return address)",
         greet_frame.ret_slot);
    note("text",   "the saved PC itself: where greet() resumes in main()",
         greet_frame.ret_value);

    return message;
}

/* Called from main(), so its frame sits below main's: which way does the
 * stack grow? Recurses first so we are several frames deep. */
static const void *deepest_frame;   /* set by deeper(), read by main() */
static const void *deepest_ret;     /* its saved PC: the recursive call site */

static void deeper(int depth)
{
    char frame_marker;
    if (depth > 0) { deeper(depth - 1); return; }
    deepest_frame = &frame_marker;
    deepest_ret   = __builtin_return_address(0);   /* points into deeper() */
    note("stack", "deeper(): local, several frames below main()", &frame_marker);
}

/* ---- main ------------------------------------------------------------ */

int main(int argc, char **argv)
{
    const char *who = (argc > 1) ? argv[1] : "world";
    int local_in_main = 0;

    char *message = greet(who);        /* heap string, addresses recorded */
    char *second  = malloc(64);        /* a later block: which way does the
                                        * heap grow? */
    deeper(2);

    const void *main_addr = (const void *)(unsigned long)main;   /* offset origin */
    note("text",   "main() entry point",                    main_addr);
    note("text",   "greet() entry point",                   (const void *)(unsigned long)greet);
    note("text",   "deeper() entry point",                  (const void *)(unsigned long)deeper);
    note("rodata", "literal \"Hello, \" (what global_literal points to)", global_literal);
    note("data",   "global_greeting[] = \"Hello, \"",       global_greeting);
    note("data",   "global_literal (the pointer variable)", &global_literal);
    note("bss",    "bss_scratch[4096] (uninitialized)",     bss_scratch);
    note("heap",   "a second, later malloc(64)",            second);
    note("stack",  "main: local 'local_in_main'",           &local_in_main);
    note("stack",  "main: argv[0] string",                  argv[0]);
    note("stack",  "main: argv array",                      argv);

    printf("pid %d   %s   \"%s\"\n\n", (int)getpid(), ARCH, message);

    printf("what it is                                          segment  address\n");
    printf("--------------------------------------------------  -------  ------------------\n");
    for (int i = 0; i < nmarks; i++)
        printf("%-50s  %-7s  %p\n", marks[i].what, marks[i].segment, marks[i].addr);

    /* Same facts, sorted low address to high: this is the process layout. */
    /* Sorted high address to low, so the printed order is the order memory is
     * conventionally drawn: stack on top, text at the bottom. */
    qsort(marks, (size_t)nmarks, sizeof marks[0], by_addr_desc);

    printf("\nhigh -> low, the way memory is drawn (stack on top, code at the bottom):\n\n");
    printf("address             segment  offset from main()  gap to next  what it is\n");
    printf("------------------  -------  ------------------  -----------  ----------------------------\n");
    for (int i = 0; i < nmarks; i++) {
        char gap[32] = "-";
        char off[32];
        long d = (long)((unsigned long)marks[i].addr - (unsigned long)main_addr);

        snprintf(off, sizeof off, "%c0x%lx", d < 0 ? '-' : '+',
                 d < 0 ? -(unsigned long)d : (unsigned long)d);

        if (i + 1 < nmarks)                       /* distance down to the next line */
            human((unsigned long)marks[i].addr - (unsigned long)marks[i + 1].addr,
                  gap, sizeof gap);

        printf("%p  %-7s  %18s  %11s  %s\n",
               marks[i].addr, marks[i].segment, off, gap, marks[i].what);
    }

    printf("\nnotes:\n");
    printf("  * global_greeting is a writable copy in .data; the literal it was\n");
    printf("    initialized from lives in read-only memory at a different address.\n");
    printf("  * the heap block outlives greet(); greet's parameter and locals do not --\n");
    printf("    they were reused by the deeper() calls made after greet() returned.\n");
    printf("  * addresses change every run: that is ASLR. Relative order does not.\n");

    /* Report the directions we actually measured, rather than asserting them. */
    /* ---- the saved program counter ---------------------------------- */

    printf("\nthe saved program counter (return address) of greet():\n\n");
    printf("  compiler says the return address is  %p   (main + 0x%lx)\n",
           greet_frame.ret_value,
           (unsigned long)((unsigned long)greet_frame.ret_value
                         - (unsigned long)main_addr));
    printf("  greet's frame pointer               %p\n", (void *)greet_frame.fp);
    printf("  frame[0], caller's saved frame ptr  %p  held %p\n",
           (void *)greet_frame.saved_fp, greet_frame.fp_live);
    printf("  frame[1], the saved PC slot         %p  held %p   %s\n",
           (void *)greet_frame.ret_slot, greet_frame.ret_live,
           greet_frame.ret_live == greet_frame.ret_value
               ? "<- found by hand, same value"
               : "<- does NOT match the builtin on this ABI");
    printf("\n  main() begins at %p, so greet() returns 0x%lx bytes into main:\n"
           "  that is the instruction after the `%s` that called it.\n",
           main_addr,
           (unsigned long)((unsigned long)greet_frame.ret_value
                         - (unsigned long)main_addr),
#if defined(__x86_64__)
           "call"
#elif defined(__aarch64__)
           "bl"
#else
           "call"
#endif
          );
    /* Same address, read now: greet returned long ago and deeper() has since
     * built its frames on top of these bytes. */
    printf("\n  that same slot, read now that greet() has returned: %p\n",
           *greet_frame.ret_slot);
    printf("%s\n", *greet_frame.ret_slot == greet_frame.ret_live
           ? "  unchanged so far -- nothing has reused those bytes yet."
           : "  overwritten. The frame is dead: deeper()'s calls reused the bytes.\n"
             "  Nothing \"frees\" a stack frame -- returning just moves the stack\n"
             "  pointer, and the next call writes over whatever was there.");

    printf("\n  deeper() recursed into itself; its innermost saved PC is %p\n",
           deepest_ret);
    printf("  (deeper + 0x%lx -- inside deeper() itself, its own call site).\n",
           (unsigned long)((unsigned long)deepest_ret
                         - (unsigned long)(unsigned long)deeper));

#if defined(__x86_64__)
    printf("\n  on x86-64 the `call` instruction PUSHES the return address onto the\n");
    printf("  stack, so it is in memory the moment the callee starts running.\n");
#elif defined(__aarch64__)
    printf("\n  on arm64 the `bl` instruction puts the return address in the LINK\n");
    printf("  REGISTER (x30), not memory; greet's prologue spills it to the stack\n");
    printf("  because greet calls other functions and would otherwise lose it. A\n");
    printf("  leaf function keeps it in x30 and never writes it down at all.\n");
#endif

    printf("\ndirections measured in this run:\n");
    printf("  * stack: main's frame is at %p, deeper()'s is at %p -- %s\n",
           (void *)&local_in_main, deepest_frame,
           (unsigned long)deepest_frame < (unsigned long)&local_in_main
               ? "lower, so the stack grows DOWN"
               : "higher, so the stack grows UP");
    printf("  * heap : greet's block is at %p, the later malloc(64) at %p -- %s\n",
           (void *)message, (void *)second,
           (unsigned long)second > (unsigned long)message ? "higher" : "lower");
    printf("           the heap segment as a whole grows up, but an allocator is\n");
    printf("           free to reuse and bin blocks, so two mallocs need not be\n");
    printf("           in call order. Compare a much larger request to see this.\n");

#ifdef __linux__
    printf("\n--- /proc/self/maps ---\n");
    { FILE *m = fopen("/proc/self/maps", "r"); char line[512];
      if (m) { while (fgets(line, sizeof line, m)) fputs(line, stdout); fclose(m); } }
#else
    printf("\n  for the kernel's view of these ranges, run:  vmmap %d\n", (int)getpid());
#endif

    free(second);
    free(message);   /* the caller owns what greet() returned */
    return 0;
}
