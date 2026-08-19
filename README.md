# WhiteRhino

WhiteRhino is a personal fork of [PCSX2](https://pcsx2.net/), the PS2
emulator — not a general-purpose PCSX2 build, and not affiliated with the
PCSX2 project. It exists for one specific job: getting **PS2 Linux**
running reliably under emulation, which stock PCSX2 was never tuned for.

## What's different from stock PCSX2

- **EE TLB/MMU accuracy fixes.** Booting a real OS (rather than a game)
  exercises the Emotion Engine's memory-management unit much harder than
  most game code does, and stock PCSX2 has a handful of bugs there that
  a game rarely trips but a kernel boot reliably does. WhiteRhino carries
  fixes for six of those, plus one in BIOS-syscall emulation.
- **`kload` and `dload`** — two ways to boot PS2 Linux directly, without a
  real console or a boot disc. `kload` is the reliable one: it stages a
  real, unmodified `kloader.elf` (built fresh from
  [kernelreloaded](https://github.com/Arawn-Davies/kernelreloaded), the
  companion bootloader project this fork is built for) alongside a kernel
  and initrd, then lets PCSX2 boot it exactly the way a real console
  would boot anything else — no shortcuts taken with CPU state. `dload`
  is the experimental alternative: a more direct boot path that pokes the
  kernel straight into guest memory, still being chased down.

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
