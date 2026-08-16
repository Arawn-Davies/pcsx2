// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "PS2Linux.h"
#include "PS2LinuxStub.h"

#include "Elfheader.h"
#include "Hw.h"
#include "Memory.h"
#include "MemoryTypes.h"
#include "R5900.h"

#include "common/Console.h"
#include "common/FileSystem.h"
#include "common/StringUtil.h"

#include "fmt/format.h"

#include <cstring>
#include <optional>
#include <vector>

#include <zlib.h>

namespace PS2Linux
{
	// ---------------------------------------------------------------------
	// The handover contract.
	//
	// Every constant here is kernelreloaded's, and every one is a value the
	// PS2 Linux kernel itself expects -- these are not choices:
	//
	//   loader/memory.h    SBIOS_START_ADDRESS 0x80001000, MAX_SBIOS_SIZE 0xf000
	//   loader/bootinfo.h  PS2_BOOTINFO_MAGIC 0x50324c42 ("P2LB")
	//                      PS2_BOOTINFO_OLDADDR (0x01fff000 | KSEG0)
	//                      PS2_BOOTINFO_MACHTYPE_PS2 0
	//   loader/loader.c    magic_string[] = "PS2b", scanned for to find the
	//                      SBIOS entry; sbios_base = SBIOS_START + (i-1)*4
	//                      the jump: entry(0, NULL, (char**)OLDADDR, NULL)
	// ---------------------------------------------------------------------

	static constexpr u32 SBIOS_LOAD_ADDR = 0x80001000;
	static constexpr u32 SBIOS_MAX_SIZE = 0xf000;
	static constexpr u32 BOOTINFO_ADDR = 0x81fff000;
	static constexpr u32 BOOTINFO_MAGIC = 0x50324c42; // "P2LB"
	static constexpr u32 MACHTYPE_PS2 = 0;

	// "PS2b" read as a little-endian word, which is how loader.c compares it
	// (it casts the char[4] to uint32_t* rather than doing a string compare).
	static constexpr u32 SBIOS_ENTRY_MAGIC = 0x62325350;

	// The kernel command line and the model string are placed immediately after
	// the bootinfo structure, inside the same page. kernelloader does the same
	// thing with its ps2_bootpage_t.
	static constexpr u32 CMDLINE_OFFSET = 256;
	static constexpr u32 CMDLINE_MAX = 1024;

	// Agreed with stub/ps2linux-stub.c. 23MB: above the kernel and initrd even
	// on a 32MB retail machine, and below the stub itself at 24MB.
	static constexpr u32 STUB_ARGS_ADDR = 0x81700000;
	static constexpr u32 STUB_ARGS_MAGIC = 0x424c5350; // "PSLB"
	static constexpr u32 STUB_MAX_MODULES = 8;

#pragma pack(push, 1)
	struct StubArgs
	{
		u32 magic;
		u32 kernel_entry;
		u32 bootinfo;
		u32 count;
		struct
		{
			u32 addr;
			u32 size;
		} mod[STUB_MAX_MODULES];
	};
#pragma pack(pop)

	// struct ps2_bootinfo, laid out for a 32-bit guest.
	//
	// It cannot be shared with the guest header because every `char *` in it is
	// 4 bytes there and 8 here. Pointer fields are u32 guest addresses.
	//
	// The one derived value is the padding after sysconf. struct ps2_sysconf is
	// `short` + 7 × `uint8_t` = 9 bytes at 2-byte alignment, so it occupies
	// bytes 32..41 and two bytes of tail padding take `magic` to 44. The guest
	// header defines PS2_BOOTINFO_OLDSIZE as offsetof(magic), so 44 is the
	// number that has to come out; the static_assert below is the check.
#pragma pack(push, 1)
	struct GuestBootInfo
	{
		u32 pccard_type;
		u32 opt_string;
		u32 reserved0;
		u32 reserved1;
		u8 boot_time[8];
		u32 mach_type;
		u32 pcic_type;
		u8 sysconf[12]; // 10 bytes of struct, 2 of tail padding
		u32 magic;
		u32 size;
		u32 sbios_base;
		u32 maxmem;
		u32 stringsize;
		u32 stringdata;
		u32 ver_vm;
		u32 ver_rb;
		u32 ver_model;
		u32 ver_ps1drv_rom;
		u32 ver_ps1drv_hdd;
		u32 ver_ps1drv_path;
		u32 ver_dvd_id;
		u32 ver_dvd_rom;
		u32 ver_dvd_hdd;
		u32 ver_dvd_path;
		u32 initrd_start;
		u32 initrd_size;
	};
#pragma pack(pop)

	static_assert(offsetof(GuestBootInfo, magic) == 44,
		"bootinfo.magic must sit at PS2_BOOTINFO_OLDSIZE (44) or the kernel reads rubbish");
	static_assert(offsetof(GuestBootInfo, sbios_base) == 52, "bootinfo layout drifted");
	static_assert(offsetof(GuestBootInfo, initrd_start) == 108, "bootinfo layout drifted");
	static_assert(sizeof(GuestBootInfo) == 116, "bootinfo layout drifted");

	static BootParams s_requested;
	static bool s_requested_valid = false;

	bool IsDirectBootRequested() { return s_requested_valid; }
	const BootParams& GetRequestedBootParams() { return s_requested; }

	void SetRequestedBootParams(BootParams params)
	{
		s_requested = std::move(params);
		s_requested_valid = !s_requested.kernel.empty();
	}

	// -----------------------------------------------------------------------

	// Host pointer for a guest *physical* address, or null if the range does
	// not fit in real memory. Main RAM is 32MB; ExtraMemory continues above it
	// and is only present when the devkit layout is enabled.
	static u8* GuestPhysPtr(u32 paddr, u32 len)
	{
		if (!eeMem || len == 0)
			return nullptr;

		const u64 end = static_cast<u64>(paddr) + len;
		if (end <= Ps2MemSize::MainRam)
			return &eeMem->Main[paddr];

		if (paddr >= Ps2MemSize::MainRam &&
			end <= static_cast<u64>(Ps2MemSize::MainRam) + Ps2MemSize::ExtraRam)
		{
			return &eeMem->ExtraMemory[paddr - Ps2MemSize::MainRam];
		}

		return nullptr;
	}

	// Strips the segment bits from a KSEG0/KSEG1 address. Everything the loader
	// deals with is in one of those two, both of which map straight onto
	// physical memory (EE Core User's Manual Fig. 5-1, note 1).
	static constexpr u32 GuestPhys(u32 vaddr) { return vaddr & 0x1FFFFFFFu; }

	static u8* GuestPtr(u32 vaddr, u32 len) { return GuestPhysPtr(GuestPhys(vaddr), len); }

	static bool GuestWrite32(u32 vaddr, u32 value)
	{
		u8* p = GuestPtr(vaddr, sizeof(u32));
		if (!p)
			return false;
		std::memcpy(p, &value, sizeof(value));
		return true;
	}

	// -----------------------------------------------------------------------

	// gzip is what every kernel and initrd in this project is shipped as, so
	// decompressing here saves staging uncompressed copies. A file that is not
	// gzipped is returned unchanged.
	static bool MaybeGunzip(std::vector<u8>& data, std::string* error)
	{
		if (data.size() < 2 || data[0] != 0x1f || data[1] != 0x8b)
			return true;

		z_stream zs = {};
		// 16 + MAX_WBITS tells zlib the input carries a gzip header.
		if (inflateInit2(&zs, 16 + MAX_WBITS) != Z_OK)
		{
			*error = "inflateInit2 failed";
			return false;
		}

		std::vector<u8> out;
		out.resize(data.size() * 4 + 0x10000);

		zs.next_in = data.data();
		zs.avail_in = static_cast<uInt>(data.size());

		for (;;)
		{
			zs.next_out = out.data() + zs.total_out;
			zs.avail_out = static_cast<uInt>(out.size() - zs.total_out);

			const int rc = inflate(&zs, Z_NO_FLUSH);
			if (rc == Z_STREAM_END)
				break;

			if (rc != Z_OK)
			{
				inflateEnd(&zs);
				*error = fmt::format("inflate failed: {}", rc);
				return false;
			}

			if (zs.avail_out == 0)
				out.resize(out.size() * 2);
		}

		out.resize(zs.total_out);
		inflateEnd(&zs);
		data = std::move(out);
		return true;
	}

	static bool ReadFile(const std::string& path, std::vector<u8>& out, std::string* error)
	{
		std::optional<std::vector<u8>> data = FileSystem::ReadBinaryFile(path.c_str());
		if (!data.has_value())
		{
			*error = fmt::format("cannot read {}", path);
			return false;
		}
		out = std::move(data.value());
		return true;
	}

	// -----------------------------------------------------------------------

	// Copies an ELF's PT_LOAD segments into guest memory and returns its entry
	// point. Written here rather than reused from Elfheader because PCSX2
	// normally lets the guest BIOS do the loading; there is nothing host-side
	// to borrow.
	//
	// `lowest`/`highest` come back as the physical extent written, which the
	// SBIOS scan needs.
	static bool LoadElfSegments(const std::vector<u8>& elf, const char* what,
		u32 limit_lo, u32 limit_hi, u32* entry, u32* lowest, u32* highest, std::string* error)
	{
		if (elf.size() < sizeof(ELF_HEADER))
		{
			*error = fmt::format("{}: too small to be an ELF", what);
			return false;
		}

		const ELF_HEADER& hdr = *reinterpret_cast<const ELF_HEADER*>(elf.data());
		if (std::memcmp(hdr.e_ident, "\177ELF", 4) != 0)
		{
			*error = fmt::format("{}: not an ELF", what);
			return false;
		}
		if (hdr.e_phnum == 0 || hdr.e_phoff == 0)
		{
			*error = fmt::format("{}: no program headers", what);
			return false;
		}
		if (static_cast<u64>(hdr.e_phoff) + static_cast<u64>(hdr.e_phnum) * sizeof(ELF_PHR) > elf.size())
		{
			*error = fmt::format("{}: program headers run past end of file", what);
			return false;
		}

		const ELF_PHR* ph = reinterpret_cast<const ELF_PHR*>(elf.data() + hdr.e_phoff);

		*lowest = 0xFFFFFFFFu;
		*highest = 0;
		bool any = false;

		for (int i = 0; i < hdr.e_phnum; i++)
		{
			if (ph[i].p_type != 1 /* PT_LOAD */ || ph[i].p_memsz == 0)
				continue;

			const u32 vaddr = ph[i].p_vaddr;
			const u32 paddr = GuestPhys(vaddr);

			if (vaddr < limit_lo || (static_cast<u64>(vaddr) + ph[i].p_memsz) > limit_hi)
			{
				*error = fmt::format("{}: segment {} at 0x{:08x}+0x{:x} is outside 0x{:08x}-0x{:08x}",
					what, i, vaddr, ph[i].p_memsz, limit_lo, limit_hi);
				return false;
			}

			u8* dst = GuestPhysPtr(paddr, ph[i].p_memsz);
			if (!dst)
			{
				*error = fmt::format("{}: segment {} at 0x{:08x} is beyond installed memory", what, i, vaddr);
				return false;
			}

			if (static_cast<u64>(ph[i].p_offset) + ph[i].p_filesz > elf.size())
			{
				*error = fmt::format("{}: segment {} runs past end of file", what, i);
				return false;
			}

			// filesz bytes from the file, then memsz-filesz of zero -- that
			// tail is .bss and the kernel assumes it is cleared.
			std::memcpy(dst, elf.data() + ph[i].p_offset, ph[i].p_filesz);
			if (ph[i].p_memsz > ph[i].p_filesz)
				std::memset(dst + ph[i].p_filesz, 0, ph[i].p_memsz - ph[i].p_filesz);

			*lowest = std::min(*lowest, paddr);
			*highest = std::max(*highest, paddr + ph[i].p_memsz);
			any = true;
		}

		if (!any)
		{
			*error = fmt::format("{}: no loadable segments", what);
			return false;
		}

		*entry = hdr.e_entry;
		return true;
	}

	// The kernel reaches the SBIOS through bootinfo.sbios_base, which points at
	// the word before the "PS2b" magic -- that word is the entry point the
	// sbios() stub jumps to. loader.c finds it by scanning the loaded image for
	// the magic and taking (i-1); the same scan is done here.
	//
	// kernelloader also walks the entry code to recover the call *table*, but
	// only so its menu can disable individual calls. The kernel never needs it.
	static bool FindSbiosBase(u32 lowest, u32 highest, u32* sbios_base, std::string* error)
	{
		for (u32 addr = lowest + 4; addr + 4 <= highest; addr += 4)
		{
			const u8* p = GuestPhysPtr(addr, sizeof(u32));
			if (!p)
				break;

			u32 word;
			std::memcpy(&word, p, sizeof(word));
			if (word != SBIOS_ENTRY_MAGIC)
				continue;

			*sbios_base = (addr - 4) | 0x80000000u;
			return true;
		}

		*error = "SBIOS magic \"PS2b\" not found -- is this a TGE sbios.elf?";
		return false;
	}

	// -----------------------------------------------------------------------

	bool DirectBoot(const BootParams& params, std::string* error)
	{
		if (!eeMem)
		{
			*error = "EE memory is not allocated";
			return false;
		}

		// ---- SBIOS -------------------------------------------------------
		std::vector<u8> sbios;
		if (!ReadFile(params.sbios, sbios, error))
			return false;
		if (!MaybeGunzip(sbios, error))
			return false;

		u32 sbios_entry = 0, sbios_lo = 0, sbios_hi = 0;
		if (!LoadElfSegments(sbios, "sbios", SBIOS_LOAD_ADDR, SBIOS_LOAD_ADDR + SBIOS_MAX_SIZE,
				&sbios_entry, &sbios_lo, &sbios_hi, error))
			return false;

		u32 sbios_base = 0;
		if (!FindSbiosBase(sbios_lo, sbios_hi, &sbios_base, error))
			return false;

		Console.WriteLn(fmt::format("PS2Linux: sbios {} bytes at 0x{:08x}, base 0x{:08x}",
			sbios_hi - sbios_lo, SBIOS_LOAD_ADDR, sbios_base));

		// ---- kernel ------------------------------------------------------
		std::vector<u8> kernel;
		if (!ReadFile(params.kernel, kernel, error))
			return false;
		if (!MaybeGunzip(kernel, error))
			return false;

		u32 kernel_entry = 0, kernel_lo = 0, kernel_hi = 0;
		// The kernel links into KSEG0 above the SBIOS. Bound it at the top of
		// installed memory rather than a fixed address so a devkit layout works.
		if (!LoadElfSegments(kernel, "kernel", 0x80000000u,
				0x80000000u + Ps2MemSize::MainRam + Ps2MemSize::ExtraRam,
				&kernel_entry, &kernel_lo, &kernel_hi, error))
			return false;

		Console.WriteLn(fmt::format("PS2Linux: kernel 0x{:08x}-0x{:08x}, entry 0x{:08x}",
			kernel_lo | 0x80000000u, kernel_hi | 0x80000000u, kernel_entry));

		// ---- initrd ------------------------------------------------------
		//
		// Placed immediately above the kernel, page-aligned, and NOT
		// decompressed: the kernel's own initrd support inflates it.
		u32 initrd_start = 0, initrd_size = 0;
		if (!params.initrd.empty())
		{
			std::vector<u8> initrd;
			if (!ReadFile(params.initrd, initrd, error))
				return false;

			const u32 phys = (kernel_hi + 0xFFFu) & ~0xFFFu;
			u8* dst = GuestPhysPtr(phys, static_cast<u32>(initrd.size()));
			if (!dst)
			{
				*error = fmt::format("initrd of {} bytes does not fit at 0x{:08x}", initrd.size(), phys);
				return false;
			}

			std::memcpy(dst, initrd.data(), initrd.size());
			initrd_start = phys | 0x80000000u;
			initrd_size = static_cast<u32>(initrd.size());

			Console.WriteLn(fmt::format("PS2Linux: initrd 0x{:08x} + 0x{:x}", initrd_start, initrd_size));
		}

		// ---- boot page ---------------------------------------------------
		u8* page = GuestPtr(BOOTINFO_ADDR, 0x1000);
		if (!page)
		{
			*error = "boot page address is not backed by memory";
			return false;
		}
		std::memset(page, 0, 0x1000);

		// The initrd is handed over on the command line, not through bootinfo.
		//
		// bootinfo.h marks initrd_start/initrd_size "Special defines, not used
		// anymore", and a working kernelloader boot bears that out -- it appends
		//
		//     rd_start=0x802d1000 rd_size=0x003fcb2f
		//
		// to whatever the user configured, and that is what the kernel reads.
		// Only the loader knows where the image landed, so this is appended
		// here rather than being the caller's problem. The bootinfo fields are
		// filled in as well, since they cost nothing and older kernels used them.
		std::string cmdline = params.cmdline;
		if (initrd_size != 0)
		{
			cmdline += fmt::format(" rd_start=0x{:08x} rd_size=0x{:08x}", initrd_start, initrd_size);
		}

		// The GS console needs asking for. Without a video= term the kernel
		// leaves tty0 on "colour dummy device 80x25" and nothing reaches the
		// screen -- kernelloader appends video=ps2fb:<mode> from its own
		// display settings. Not defaulted here: the mode has to match the
		// display, and crtmode=/xmode= belong with it, so it stays the
		// caller's to pass. Say so when it is missing rather than leaving a
		// black screen unexplained.
		if (cmdline.find("video=") == std::string::npos)
		{
			Console.Warning("PS2Linux: no video= in the command line, so the GS console will not start. "
							"Add e.g. \"crtmode=pal1 xmode=PAL video=ps2fb:pal,640x480-32\" for on-screen output.");
		}

		if (cmdline.size() >= CMDLINE_MAX)
		{
			*error = "kernel command line is too long";
			return false;
		}
		std::memcpy(page + CMDLINE_OFFSET, cmdline.c_str(), cmdline.size() + 1);

		GuestBootInfo bi = {};
		bi.magic = BOOTINFO_MAGIC;
		bi.size = sizeof(GuestBootInfo);
		bi.mach_type = MACHTYPE_PS2;
		bi.sbios_base = sbios_base;
		bi.opt_string = BOOTINFO_ADDR + CMDLINE_OFFSET;
		bi.initrd_start = initrd_start;
		bi.initrd_size = initrd_size;

		// maxmem is what arch/mips/ps2/prom.c turns into the memory map:
		//   add_memory_region(0, ps2_bootinfo->maxmem & PAGE_MASK, BOOT_MEM_RAM)
		// Telling it more than exists hands out pages that are not there, so it
		// defaults to the real size.
		bi.maxmem = params.maxmem ? params.maxmem
								  : (memGetExtraMemMode() ? (Ps2MemSize::MainRam + Ps2MemSize::ExtraRam)
														  : Ps2MemSize::MainRam);

		std::memcpy(page, &bi, sizeof(bi));

		Console.WriteLn(fmt::format("PS2Linux: bootinfo at 0x{:08x}, maxmem {} MB, cmdline \"{}\"",
			BOOTINFO_ADDR, bi.maxmem / _1mb, cmdline));

		// ---- SIF RPC address ---------------------------------------------
		//
		// The SBIOS needs the IOP's published SIF RPC receive address, and
		// kernelloader writes it to a fixed word just before jumping:
		//
		//     iopaddr = SifGetReg(0x80000000);      loader.c:2517
		//     *((u32 *) 0x80001008) = iopaddr;      loader.c:2758
		//     "Patched sbios_iopaddr 0x00019600"
		//
		// SifGetReg is sceSifGetReg, EE syscall 0x7a -- a BIOS kernel call, not
		// a hardware read. The EE User's Manual register map documents exactly
		// one SIF register on the EE side, SB_SMFLG at 0x1000f230, so the value
		// exists only because the IOP's SIF driver published it. That is the
		// whole reason this runs from eeloadHook() rather than straight after
		// cpuReset(): by EELOAD the BIOS has booted and the IOP is up.
		//
		// Not running guest code, we take it from the SBUS register the IOP
		// writes it into rather than making the syscall.
		{
			const u32 f200 = psHu32(SBUS_F200);
			const u32 f210 = psHu32(SBUS_F210);
			const u32 f240 = psHu32(SBUS_F240);
			const u32 f260 = psHu32(SBUS_F260);

			Console.WriteLn(fmt::format(
				"PS2Linux: SBUS MSCOM={:08x} SMCOM={:08x} F240={:08x} F260={:08x}",
				f200, f210, f240, f260));

			const u32 iopaddr = f210;
			if (!GuestWrite32(0x80001008, iopaddr))
			{
				*error = "cannot write sbios_iopaddr";
				return false;
			}
			Console.WriteLn(fmt::format("PS2Linux: sbios_iopaddr = 0x{:08x}", iopaddr));
		}

		// ---- IOP modules, and the stub that loads them -------------------
		//
		// Putting a module on the IOP is an RPC to its LOADFILE, so it cannot
		// be done from here -- the honest way is to be an EE program and ask
		// the BIOS, which is what kernelloader does and what the stub does.
		//
		// Without the interrupt relay the kernel faults during early init,
		// reaching into its IOP-memory window at ~0x70000000 with nothing
		// behind it. That fault is what this exists to prevent.
		StubArgs stub_args = {};
		stub_args.magic = STUB_ARGS_MAGIC;
		stub_args.kernel_entry = kernel_entry;
		stub_args.bootinfo = BOOTINFO_ADDR;
		stub_args.count = 0;

		u32 mod_addr = ((initrd_size ? (GuestPhys(initrd_start) + initrd_size) : kernel_hi) + 0xFFFu) & ~0xFFFu;

		for (const std::string& path : params.iop_modules)
		{
			if (stub_args.count >= STUB_MAX_MODULES)
			{
				*error = fmt::format("more than {} IOP modules", STUB_MAX_MODULES);
				return false;
			}

			std::vector<u8> irx;
			if (!ReadFile(path, irx, error))
				return false;

			u8* dst = GuestPhysPtr(mod_addr, static_cast<u32>(irx.size()));
			if (!dst)
			{
				*error = fmt::format("IOP module {} does not fit at 0x{:08x}", path, mod_addr);
				return false;
			}
			std::memcpy(dst, irx.data(), irx.size());

			stub_args.mod[stub_args.count].addr = mod_addr | 0x80000000u;
			stub_args.mod[stub_args.count].size = static_cast<u32>(irx.size());
			stub_args.count++;

			Console.WriteLn(fmt::format("PS2Linux: iop module {} at 0x{:08x} + 0x{:x}",
				path, mod_addr | 0x80000000u, irx.size()));

			mod_addr = (mod_addr + static_cast<u32>(irx.size()) + 0xFFFu) & ~0xFFFu;
		}

		u8* argp = GuestPtr(STUB_ARGS_ADDR, sizeof(StubArgs));
		if (!argp)
		{
			*error = "stub argument block is not backed by memory";
			return false;
		}
		std::memcpy(argp, &stub_args, sizeof(stub_args));

		std::vector<u8> stub(std::begin(s_ps2linux_stub_elf), std::end(s_ps2linux_stub_elf));
		u32 stub_entry = 0, stub_lo = 0, stub_hi = 0;
		if (!LoadElfSegments(stub, "stub", 0x80000000u,
				0x80000000u + Ps2MemSize::MainRam + Ps2MemSize::ExtraRam,
				&stub_entry, &stub_lo, &stub_hi, error))
			return false;

		Console.WriteLn(fmt::format("PS2Linux: stub 0x{:08x}-0x{:08x}, entry 0x{:08x}, {} module(s)",
			stub_lo | 0x80000000u, stub_hi | 0x80000000u, stub_entry, stub_args.count));

		// ---- handover ----------------------------------------------------
		//
		// Into the stub, not the kernel: it starts the IOP modules and then
		// enters the kernel itself, calling it exactly as kernelloader does
		//     entry(0, NULL, (char **) PS2_BOOTINFO_OLDADDR, NULL)
		// so the boot page arrives in a2.
		cpuRegs.pc = stub_entry;
		cpuRegs.GPR.n.a0.UD[0] = 0;
		cpuRegs.GPR.n.a1.UD[0] = 0;
		cpuRegs.GPR.n.a2.UD[0] = 0;
		cpuRegs.GPR.n.a3.UD[0] = 0;

		Console.WriteLn(fmt::format("PS2Linux: entering stub at 0x{:08x}, kernel 0x{:08x}",
			stub_entry, kernel_entry));
		return true;
	}
} // namespace PS2Linux
