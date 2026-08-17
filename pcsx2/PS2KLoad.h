// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>

// kload: boot PS2 Linux by staging kernelreloaded's own kloader.elf (bundled
// as a resource, see whiterhino/tools/build-kloader-resource.sh) next to a
// user-supplied kernel/initrd and a generated config.txt, then handing the
// result to PCSX2's normal ELF-boot path -- the same one a manual
// kloader.elf + config.txt boot already uses.
//
// Deliberately not PS2Linux.cpp/DirectBoot(): that path (dload) re-implements
// pieces of what EELOAD's real exec sets up for a launched program, and has
// been hard to get right for exactly that reason. kload reuses the real path
// instead, so it needs none of DirectBoot()'s CPU-state code -- see the
// kload/dload comparison in kernelreloaded's project history for why both
// exist rather than one replacing the other.
namespace PS2KLoad
{
	struct BootParams
	{
		std::string kernel;   // required; vmlinux, optionally gzipped
		std::string initrd;   // optional; gzipped is fine, kernelloader unpacks it
		std::string cmdline;  // optional; goes into config.txt's KernelParameter

		// Selects which bundled kloader ELF to stage (kloader.elf vs
		// kloader-instant.elf, see Stage()'s own comment) and, separately,
		// config.txt's AutoBootTime, verified against loader/main.cpp's own
		// countdown loop: 0 means "off" -- always interactive, never
		// auto-boots, the opposite of what the name suggests. false selects
		// 3 (a countdown worth reading); true selects the negative sentinel
		// (-1) -- see main.cpp's `if (loaderConfig.autoBootTime < 0)` branch,
		// which skips the countdown screen entirely and boots straight
		// through. loaderConfig.instantBoot (set by kloader-instant.elf,
		// unconditionally, at the very first line of main()) is a separate
		// thing -- it only moves bootlogBegin() earlier and is checked
		// nowhere else, so it does NOT gate or replace this AutoBootTime
		// check. -1 is still required for the instant path to actually skip
		// the menu; without it the loader sits waiting for pad input.
		bool instant = false;
	};

	// Stages kloader.elf + kernel + initrd + a generated config.txt into a
	// fresh temp directory and returns the staged kloader.elf's path, ready
	// to assign to AutoBoot(autoboot)->filename. Returns an empty string and
	// fills `error` on failure.
	std::string Stage(const BootParams& params, std::string* error);
} // namespace PS2KLoad
