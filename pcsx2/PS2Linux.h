// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "common/Pcsx2Defs.h"

#include <string>
#include <vector>

// Direct PS2 Linux boot, without kernelloader.
//
// WhiteRhino exists to run Linux, so requiring a separate loader ELF to be
// staged, configured and stepped through before the kernel starts is friction
// with no emulation value. This does what kernelloader's real_loader() does at
// handover -- and nothing else.
//
// It is not an SBIOS. The real TGE sbios.elf is loaded and used unchanged, so
// every call the kernel makes still runs guest code. Only the *loading* is
// done here.
//
// The handover contract is kernelreloaded's, taken from loader/memory.h,
// loader/bootinfo.h and loader/loader.c. See PS2Linux.cpp for the citations.
namespace PS2Linux
{
	struct BootParams
	{
		std::string kernel;   // vmlinux, optionally gzipped
		std::string initrd;   // optional; gzipped is fine, it is not decompressed
		std::string sbios;    // TGE sbios.elf
		std::string cmdline;  // kernel command line
		u32 maxmem = 0;       // 0 = use the real machine's RAM size

		// IOP modules to start before entering the kernel, in order. The
		// interrupt relay is the one that matters -- without it the kernel's
		// SIF path has nothing answering and it faults during early init. These
		// cannot be loaded from the host: putting a module on the IOP is an RPC
		// to its LOADFILE, so a small EE stub does it. See PS2Linux.cpp.
		std::vector<std::string> iop_modules;
	};

	// Returns false and fills `error` on any problem. On success the EE is left
	// at the kernel entry point with the boot page built, ready to run.
	bool DirectBoot(const BootParams& params, std::string* error);

	// True when -kernel was given, so the normal BIOS path should be skipped.
	bool IsDirectBootRequested();
	const BootParams& GetRequestedBootParams();
	void SetRequestedBootParams(BootParams params);
} // namespace PS2Linux
