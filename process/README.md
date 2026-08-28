# process — where a running program keeps its stuff

A single C program, `memmap.c`, that prints the address of every kind of thing
a process holds — code, constants, globals, heap, stack frames, and the saved
program counter — then sorts those addresses to show the layout.

## Running it

Two ways. **Use the container for anything you want to compare with someone
else's output**, because the numbers below are x86-64 Linux numbers:

```
./build.sh              # build an x86-64 Linux image (emulated on Apple Silicon)
./run.sh                # ./memmap world, inside that image
./run.sh Warren
./run.sh --no-aslr      # kernel randomization off (see "Reproducibility")
./run.sh --shell        # poke around in the container
```

Native, on whatever this machine is:

```
make        # build
make run    # ./memmap world
make twice  # run twice: the addresses move, the layout doesn't (ASLR)
make symbols# what the linker decided, before the program ever runs
make sizes  # section sizes in the executable image
make clean
```

On an Apple Silicon Mac the native build is arm64 and the container is x86-64.
Run both and compare — the differences are the point of the last section.

## What it shows

| thing | segment | lives until |
| --- | --- | --- |
| `main()`, `greet()`, `deeper()` entry points | text | process exit (read-only, executable) |
| the literal `"Hello, "` | rodata | process exit (read-only) |
| `global_greeting[] = "Hello, "` | data | process exit (writable) |
| `global_literal` (the *pointer*, not the string) | data | process exit |
| `bss_scratch[4096]` | bss | process exit (zero-filled, 0 bytes on disk) |
| `"Hello, " + who` from `malloc` | heap | until you `free()` it |
| `greet`'s parameter `who` and locals | stack | until `greet` returns |
| `greet`'s saved frame pointer and **saved PC** | stack | until `greet` returns |
| `argv` array and `argv[0]` string | stack | process exit |

The two `"Hello, "`s are the point of the exercise. `global_greeting` is a
writable array *initialized from* a literal, so the literal sits in read-only
memory and a copy sits in `.data` — two different addresses for the same eight
bytes. The heap string is a third copy, built at runtime.

Likewise `greet`'s parameter `who` has two addresses worth distinguishing:
`who` (the pointer variable, on greet's stack frame) and `*who` (the characters
it points at, wherever the caller put them — for `./run.sh Warren` that's the
argv area near the top of the stack).

## Reading the output

The second table is sorted high address to low, so the print order *is* the
memory order as it is conventionally drawn — stack on top, code at the bottom —
with each address also given as a signed hex offset from `main()`:

```
address         segment  offset from main()  gap to next  what it is
0x7ffffffcaf76  stack       +0x2aaaaaa7595d      654.0 B  argv[0] string
0x7ffffffcab94  stack       +0x2aaaaaa7557b      572.0 B  main: local_in_main
0x7ffffffca958  stack       +0x2aaaaaa7533f        8.0 B  greet: saved PC slot
0x7ffffffca950  stack       +0x2aaaaaa75337       24.0 B  greet: saved frame pointer
0x7ffffffca928  stack       +0x2aaaaaa7530f       57.0 B  greet: parameter 'who'
0x7ffffffca8ef  stack       +0x2aaaaaa752d6     42.7 TiB  deeper(): local
0x55555555a2a0  heap                +0x4c87      8.4 KiB  malloc'd "Hello, " + who
0x555555558120  bss                 +0x2b07      128.0 B  bss_scratch[4096]
0x555555558090  data                +0x2a77      8.1 KiB  global_greeting[]
0x555555556008  rodata               +0x9ef      2.4 KiB  literal "Hello, "
0x555555555665  text                  +0x4c       76.0 B  saved PC: greet() resumes here
0x555555555619  text                   +0x0       88.0 B  main() entry point
0x5555555553e6  text                 -0x233            -  greet() entry point
```

Top to bottom: the stack, a 42.7 TiB gap, the heap, then globals, constants and
code. The gap is unmapped address space: the heap grows into it from below
(upward in address) and the stack grows into it from above (downward). The
program also dumps `/proc/self/maps` on Linux, which shows the same ranges as
the kernel sees them.

The offset column is the same information relative to `main()`. Everything
statically laid out by the linker is a small offset away (`greet` is 0x233
*before* `main`; the globals a few KiB after), because it all comes out of the
same executable image. The heap is kilobytes away and the stack is terabytes
away, and on a native run those two distances change every time while the small
ones never do: the image is one contiguous mapping, but where the kernel puts
the heap and stack relative to it is randomized.

## The saved program counter

When `main` calls `greet`, the address to come back to has to be written down
somewhere. That is the saved PC, or return address, and it is the reason a
`return` statement knows where to go.

```
  compiler says the return address is  0x555555555665   (main + 0x4c)
  greet's frame pointer               0x7ffffffca950
  frame[0], caller's saved frame ptr  0x7ffffffca950  held 0x7ffffffcabd0
  frame[1], the saved PC slot         0x7ffffffca958  held 0x555555555665   <- found by hand

  main() begins at 0x555555555619, so greet() returns 0x4c bytes into main:
  that is the instruction after the `call` that called it.
```

The program gets the value two ways and checks they agree: `__builtin_return_
address(0)`, which the compiler answers, and by hand — `__builtin_frame_
address(0)` gives greet's frame pointer, and both the x86-64 SysV ABI and the
AArch64 AAPCS put a two-word frame record there: `[0]` is the caller's saved
frame pointer, `[1]` is the saved PC. Chaining `[0]` from frame to frame is
exactly how a debugger produces a backtrace.

Note the value lands *inside* `main`, 0x4c bytes past its entry — not at
`main`'s start. It is the address of the instruction after the call.

### Stack frames are not freed, just abandoned

`main` reads that same slot once more, after `greet` has returned:

```
  that same slot, read now that greet() has returned: 0x555555555c65
  overwritten. The frame is dead: deeper()'s calls reused the bytes.
```

Nothing cleans a stack frame up. Returning moves the stack pointer, and the
next call writes over whatever was there — here, `deeper()`'s own saved PC.
This is why returning a pointer to a local is a bug and returning the `malloc`d
string is not.

### x86-64 vs arm64

This is the part that actually differs between the two machines, and why the
container is worth the trouble:

- **x86-64**: `call` *pushes* the return address onto the stack as part of the
  instruction. It is in memory before the callee runs a single instruction.
- **arm64**: `bl` puts the return address in the **link register** (`x30`) and
  touches no memory. `greet`'s prologue spills `x30` to the stack because
  `greet` calls other functions, which would overwrite it. A leaf function
  keeps it in the register and never writes it down at all.

Run `./run.sh` and `make run` side by side: the saved PC exists in both, but on
arm64 it is on the stack because the compiler chose to put it there, and on
x86-64 because the hardware put it there.

## Reproducibility

Under `./run.sh`, the code, rodata, data, bss and heap addresses come out
identical on every run and on every machine — the emulator does not randomize
the PIE base, so `main` is at `0x555555555619` for everyone. Stack *frame*
internals are identical too (the offsets between `greet`'s locals, its saved
frame pointer, and its saved PC slot).

What still moves a little is the base of the stack: the argv/environment area
at the very top shifts by a couple hundred bytes between runs, because the
x86-64 emulation layer pads it. `./run.sh --no-aslr` turns off the kernel's
randomization (via `setarch -R`, which needs `--security-opt seccomp=unconfined`
because Docker's default seccomp profile blocks the `personality` syscall) and
does not remove that last bit of jitter. So compare stack *offsets*, not
absolute stack addresses.

## Things to try

- Run it twice natively (`make twice`). Every address changes — that is ASLR.
  The order, the gaps, and the offsets within the image do not change.
- `make symbols` — `nm` shows `_main`, `_greet`, `_global_greeting`,
  `_bss_scratch` and which section each landed in, decided at link time.
- `make sizes` — `.bss` contributes 4096 bytes of runtime memory but nothing
  to the file on disk; only its size is recorded.
- Delete the `free(message)` in `main` and run under `leaks -atExit -- ./memmap`
  (macOS) or `valgrind ./memmap` (Linux). The heap block is the only thing here
  that can leak; nothing else needed freeing.
- Try to write through `global_literal`: add `global_literal[0] = 'J';` (you'll
  need to drop the `const`). It compiles, then segfaults — the rodata page is
  mapped read-only by the kernel. `global_greeting[0] = 'J'` is fine.
- Overwrite `*greet_frame.ret_slot` from inside `greet` before returning, and
  watch control return somewhere it should not. That is a stack smash, by hand.
- Recurse without a base case and watch the stack run into its guard page —
  `/proc/self/maps` shows it as the `---p` (no permissions) range just below
  the stack.
- Build with `-O2` and see the frame-pointer trick stop working: the compiler
  may omit the frame pointer, inline `greet`, or keep locals only in registers,
  where they have no address at all.
