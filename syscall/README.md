# Crossing the barrier

The kernel is not a library you call. **There is no address to jump to.** You
put a number in a register, execute one instruction, and the *hardware* decides
where control lands — a vector table the kernel installed at boot, in memory
your process cannot write.

That is the whole barrier, and it is why the mitigations from last week matter:
NX, ASLR and canaries are only worth anything *because* an attacker cannot pick
the kernel entry point. If they could, they would never bother smashing a frame.

    make run          # both demos
    make disasm       # find the trap instruction in your own binary
    ./build.sh        # the same source, built for x86-64 AND arm64 Linux
    ./run.sh          # run both — one natively, one emulated

## 1. `raw.c` — the same idea, four ways

Read the `#if` blocks as a survey of **arbitrary choices around one fixed
idea**. What differs is convention; what doesn't is that a number and an
instruction replace an address.

| | number in | instruction | `write` is |
|---|---|---|---|
| Linux arm64 | `x8` | `svc #0` | 64 |
| macOS arm64 | `x16` | `svc #0x80` | 4 |
| Linux x86-64 | `rax` | `syscall` | 1 |
| macOS x86-64 | `rax`, `\|0x2000000` | `syscall` | 4 |

**Notice what the file does not contain: a return address.** On arm64 `svc`
parks it in `ELR_EL1` — a system register at a privilege level this code cannot
reach. That is the same trick as `LR` for an ordinary call, one level up and
out of your hands.

Then `make disasm` and find the identical instruction inside libc's `write()`.
Same instruction. One of them you wrote.

### Both Linux rows at once

`make disasm` shows you one row of that table — whichever ISA you happen to
own. The container shows you two:

    ./build.sh                    # builds raw.c twice: linux/amd64, linux/arm64
    ./run.sh                      # runs both
    ./run.sh amd64 make disasm    # or just one

    linux/arm64 · native      d4000001    svc  #0x0
    linux/amd64 · emulated    0f 05       syscall

One source file, one identical line of output, two different instructions —
which is the claim in the table, demonstrated rather than asserted.

One of those containers is your own ISA and one is not. The foreign one still
traps correctly, because a binfmt handler is translating its instruction into a
real one on the host kernel: Rosetta on an Apple Silicon Mac, qemu elsewhere.
That is worth a sentence in your writeup. Take your **timings** from the native
side, though — on the emulated side you are measuring the translator.

## 2 and 3. `cost.c` — weigh it

You cannot see the boundary. You can time it. Measured on an M-series Mac:

    function call         6.4 ns
    getpid()  [trap]    189.0 ns    29.5x the call
    clock_gettime()      25.3 ns     3.9x the call

The first two are the lecture. **The third is the assignment: why isn't it 30x
too?** Run it before reading on.

<details><summary>after you have your own numbers</summary>

`clock_gettime` mostly **does not trap**. The kernel maps a page of code and
data into every process — the **vDSO** on Linux, `commpage` on Darwin — so the
answer can be read in userspace. The barrier is expensive enough that the OS
builds an escape hatch for the calls you make most.

**The best system call is the one that isn't one.** That is also why `io_uring`
exists: batch the crossings instead of paying per request.
</details>

## Homework — make `fork`/`exec` explicit

Take the `fork`/`exec` example and replace the libc calls with raw syscalls, the
way `raw.c` does. Report the numbers you used and how you found them.

**One warning, and it is the interesting part of this assignment.**

On **Linux this works**: syscall numbers are a *stable ABI*. Number 1 on x86-64
will mean `write` forever, which is why a binary from 2010 still runs today.

On **macOS it will not**, and you should try anyway so you can say precisely how
it fails. Darwin's numbers are **not a supported interface** — Apple's boundary
is `libSystem`, not the kernel, and it reserves the right to renumber. `fork` in
particular is not a bare syscall there: libSystem does real work around it, and
a raw `fork` returns a child in a state the runtime does not expect.

**So the barrier is not drawn in the same place on the two systems.** Linux
draws it at the kernel; macOS draws it at a library, and treats the kernel as an
implementation detail. Same concept, different layer — and it changes what a
program is allowed to depend on.

**Do the work in the Linux container** — `./build.sh`, then `./run.sh --shell
amd64` to work inside it. Mac users get a Linux userland and stable numbers;
then write a paragraph on what that container is actually doing for you here,
which is the second half of the assignment.

## What to hand in

1. Your three timings, from your machine.
2. A paragraph explaining the third number.
3. `fork`/`exec` with explicit syscalls, running in the container.
4. A paragraph on why it would not be a reasonable thing to ship on macOS.
