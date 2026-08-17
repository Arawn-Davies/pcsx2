// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "PS2KLoad.h"

#include "Config.h"
#include "Host.h"
#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/Path.h"

#include "fmt/format.h"

#include <chrono>
#include <filesystem>
#include <fstream>

namespace PS2KLoad
{
	std::string Stage(const BootParams& params, std::string* error)
	{
		// bin/resources/kernelreloaded/{kloader,kloader-instant}.elf, built
		// and placed there by tools/build-kloader-resource.sh before CMake
		// ever configures -- CMakeLists.txt globs bin/resources/ at
		// configure time and copies whatever it finds, so nothing in the
		// build itself needs to know these files exist.
		//
		// kloader-instant.elf has bootlogBegin() called at the first line of
		// main(), before config.txt is even readable -- the earliest point
		// possible, which the normal kloader.elf's runtime AutoBootTime=-1
		// path (config-driven, so gated on config.txt being readable first)
		// cannot reach on its own. See config.mk's INSTANT_BOOT_DEFAULT.
		const std::string kloaderElf = Path::Combine(EmuFolders::Resources,
			params.instant ? "kernelreloaded/kloader-instant.elf" : "kernelreloaded/kloader.elf");
		if (!FileSystem::FileExists(kloaderElf.c_str()))
		{
			*error = fmt::format("bundled {} not found at {} -- run tools/build-kloader-resource.sh first",
				params.instant ? "kloader-instant.elf" : "kloader.elf", kloaderElf);
			return {};
		}
		if (!FileSystem::FileExists(params.kernel.c_str()))
		{
			*error = fmt::format("kernel not found: {}", params.kernel);
			return {};
		}
		if (!params.initrd.empty() && !FileSystem::FileExists(params.initrd.c_str()))
		{
			*error = fmt::format("initrd not found: {}", params.initrd);
			return {};
		}

		// host: resolves to whatever directory the booted ELF is in
		// (IopBios.cpp's Hle_SetHostRoot sets it from the boot filename), so
		// kloader.elf, the kernel and the initrd all have to be staged
		// together next to a generated config.txt -- the same layout a
		// manual kernelloader boot uses, just assembled here instead of by
		// hand.
		std::error_code ec;
		const std::filesystem::path tempRoot = std::filesystem::temp_directory_path(ec);
		if (ec)
		{
			*error = fmt::format("could not determine a temp directory: {}", ec.message());
			return {};
		}
		const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
		const std::string stageDir = (tempRoot / fmt::format("whiterhino-kload-{}", now)).string();

		if (!FileSystem::CreateDirectoryPath(stageDir.c_str(), true))
		{
			*error = fmt::format("could not create staging directory: {}", stageDir);
			return {};
		}

		const std::string stagedKloader = Path::Combine(stageDir, "kloader.elf");
		const std::string stagedKernel = Path::Combine(stageDir, std::string(Path::GetFileName(params.kernel)));

		if (!FileSystem::CopyFilePath(kloaderElf.c_str(), stagedKloader.c_str(), true) ||
			!FileSystem::CopyFilePath(params.kernel.c_str(), stagedKernel.c_str(), true))
		{
			*error = fmt::format("failed to stage kloader.elf/kernel into {}", stageDir);
			return {};
		}

		std::string stagedInitrd;
		if (!params.initrd.empty())
		{
			stagedInitrd = Path::Combine(stageDir, std::string(Path::GetFileName(params.initrd)));
			if (!FileSystem::CopyFilePath(params.initrd.c_str(), stagedInitrd.c_str(), true))
			{
				*error = fmt::format("failed to stage initrd into {}", stageDir);
				return {};
			}
		}

		std::string configTxt = fmt::format("KernelFileName=host:{}\n", Path::GetFileName(stagedKernel));
		if (!stagedInitrd.empty())
			configTxt += fmt::format("InitrdFileName=host:{}\n", Path::GetFileName(stagedInitrd));
		configTxt += fmt::format("KernelParameter={}\n",
			params.cmdline.empty() ? "root=/dev/ram0 rw ramdisk_size=16384 romcons console=tty0 console=romcons init=/minish" : params.cmdline);
		// Corrected back to -1 after a wrong deduction: loaderConfig.instantBoot
		// (set unconditionally by kloader-instant.elf, before main.cpp's own
		// `if (loaderConfig.autoBootTime < 0)` block) is checked NOWHERE else
		// in main.cpp -- grep confirms it. It only moves bootlogBegin() to the
		// earliest possible line; it does not gate or skip the AutoBootTime
		// check later in main(). That later check is still what has to fire,
		// and only `< 0` takes the "skip straight through" branch (renormalizing
		// itself back to a valid 0 as its first statement, so there is no
		// stray-negative risk after all). AutoBootTime=0 falls into neither
		// branch and leaves the loader sitting at an interactive menu waiting
		// for pad input -- confirmed the hard way as "pad 1 initalization
		// failed!" (loader/pad.c) with no real controller behind it.
		configTxt += fmt::format("AutoBootTime={}\n", params.instant ? -1 : 3);

		// kernelreloaded defaults to retail-safe: plain 32MB unless it was
		// compiled with FAKE_MAXMEM_MB, a build-time guess. PCSX2 does not
		// have to guess -- it already knows whether the 128MB devkit map
		// (ExtraMemory=true) is actually backing that memory or not, so it
		// says so here instead of leaving kernelloader to assume anything.
		// A boolean, not a byte count: kernelreloaded already knows the one
		// number that matters (128MB, the T10K devkit layout PCSX2's
		// ExtraMemory=true actually maps) -- see loaderConfig.enableExtraMem's
		// comment in loader.h.
		//
		// Read directly from the ini (Host::GetBaseBoolSettingValue), not
		// EmuConfig.Cpu.ExtraMemory and not memGetExtraMemMode(): both
		// reflect VMManager.cpp's memSetExtraMemMode(EmuConfig.Cpu.ExtraMemory),
		// which only runs once the VM actually starts. Stage() runs before
		// that (see this function's own call site comment in QtHost.cpp), so
		// both read as unset regardless of the real ini value -- confirmed
		// the hard way as "EmuConfig.Cpu.ExtraMemory=false" with
		// ExtraMemory=true sitting in PCSX2.ini the whole time. The ini
		// itself has no such ordering dependency.
		const bool enableExtraMem = Host::GetBaseBoolSettingValue("EmuCore/CPU", "ExtraMemory", false);
		configTxt += fmt::format("EnableExtraMem={}\n", enableExtraMem ? 1 : 0);

		const std::string configPath = Path::Combine(stageDir, "config.txt");
		std::ofstream configFile(configPath, std::ios::binary);
		if (!configFile.is_open() || !(configFile << configTxt))
		{
			*error = fmt::format("failed to write {}", configPath);
			return {};
		}
		configFile.close();

		Console.WriteLn(fmt::format("PS2KLoad: staged kloader.elf at {}", stagedKloader));
		return stagedKloader;
	}
} // namespace PS2KLoad
