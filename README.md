# WhiteRhino

WhiteRhino is a personal fork of [PCSX2](https://pcsx2.net/), the PS2
emulator — not a general-purpose PCSX2 build, and not affiliated with the
PCSX2 project. It exists for one specific job: getting **guest OSes**
(PS2 Linux, and since 2026-08-22 NetBSD's `playstation2` port) running
reliably under emulation, which stock PCSX2 was never tuned for. Its EE
interpreter had only ever been exercised by Linux guests until NetBSD
started booting through it — a bug found while porting a non-Linux guest
may be a WhiteRhino gap, not a guest-kernel bug; check here first.

## What's different from stock PCSX2

- **EE TLB/MMU accuracy fixes.** Booting a real OS (rather than a game)
  exercises the Emotion Engine's memory-management unit much harder than
  most game code does, and stock PCSX2 has a handful of bugs there that
  a game rarely trips but a kernel boot reliably does. WhiteRhino carries
  fixes for six of those, plus one in BIOS-syscall emulation.
- **`kload` and `dload`** — two ways to boot a guest OS directly, without a
  real console or a boot disc. `kload` is the reliable one: it stages a
  real, unmodified `kloader.elf` (built fresh from
  [kernelreloaded](https://github.com/Arawn-Davies/kernelreloaded), the
  companion bootloader project this fork is built for) alongside a kernel
  and initrd, then lets PCSX2 boot it exactly the way a real console
  would boot anything else — no shortcuts taken with CPU state. `dload`
  is the experimental alternative: a more direct boot path that pokes the
  kernel straight into guest memory, still being chased down.
- **A crash loop reports itself instead of taking the emulator down with
  it.** A guest stuck re-executing the same faulting instruction forever
  used to be able to do two bad things: flood the log/console to hundreds
  of MB in under a second (`common/Console.cpp` now caps total log
  messages per session, across every sink — file, console, debug output,
  host/Qt callback — regardless of which caller is spamming), and hang
  indefinitely on shutdown (`Host::RequestVMShutdown()` needs the CPU
  thread to reach a cooperative stop check that a spinning interpreter
  never reaches; the crash-loop detectors in `pcsx2/R5900.cpp` and
  `pcsx2/R5900OpcodeImpl.cpp` now call `HostSys.cpp`'s
  `AlertUserAndExit()` instead — a synchronous native alert shown
  directly on the calling thread, no Qt, no cooperative check needed,
  then an unconditional exit).

If you just want to play PS2 games, you want
[upstream PCSX2](https://github.com/PCSX2/pcsx2) — it's actively
developed, well documented, and this fork doesn't try to compete with it.

## Building

This fork is built the same way PCSX2 itself is; see
[PCSX2's own build documentation](https://pcsx2.net/docs/contributing/)
for the general instructions. Windows is where this fork gets exercised
day to day, and `tools/build-windows.ps1` /
`tools/build-kloader-resource.sh` handle the extra step `kload` needs —
building kernelreloaded's `kloader.elf` fresh and embedding it as a
resource.

You'll still need a BIOS dump from a legitimately-owned PS2, same as
stock PCSX2.
