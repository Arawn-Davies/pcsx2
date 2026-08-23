// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"

#include <cstdlib>
#include <vector>

#include "common/HostSys.h"
#include "common/StringUtil.h"
#include "ps2/BiosTools.h"
#include "R5900.h"
#include "PS2Linux.h"
#include "Host.h"
#include "R3000A.h"
#include "ps2/pgif.h" // pgif init
#include "VUmicro.h"
#include "COP0.h"
#include "MTVU.h"
#include "VMManager.h"

#include "Hardware.h"
#include "IPU/IPUdma.h"

#include "Elfheader.h"
#include "CDVD/CDVD.h"
#include "Patch.h"
#include "GameDatabase.h"
#include "GSDumpReplayer.h"

#include "DebugTools/Breakpoints.h"
#include "DebugTools/MIPSAnalyst.h"
#include "DebugTools/SymbolGuardian.h"
#include "R5900OpcodeTables.h"
#include "DebugTools/Debug.h"

#include "fmt/format.h"

using namespace R5900;	// for R5900 disasm tools

s32 EEsCycle;		// used to sync the IOP to the EE
u32 EEoCycle;

alignas(16) cpuRegistersPack _cpuRegistersPack;
alignas(16) tlbs tlb[48];
cachedTlbs_t cachedTlbs;

R5900cpu *Cpu = NULL;

static constexpr uint eeWaitCycles = 3072;

bool eeEventTestIsActive = false;
EE_intProcessStatus eeRunInterruptScan = INT_NOT_RUNNING;

u32 g_eeloadMain = 0, g_eeloadExec = 0, g_osdsys_str = 0;

/* I don't know how much space for args there is in the memory block used for args in full boot mode,
but in fast boot mode, the block we use can fit at least 16 argv pointers (varies with BIOS version).
The second EELOAD call during full boot has three built-in arguments ("EELOAD rom0:PS2LOGO <ELF>"),
meaning that only the first 13 game arguments supplied by the user can be added on and passed through.
In fast boot mode, 15 arguments can fit because the only call to EELOAD is "<ELF> <<args>>". */
const int kMaxArgs = 16;
uptr g_argPtrs[kMaxArgs];
#define DEBUG_LAUNCHARG 0 // show lots of helpful console messages as the launch arguments are passed to the game

void cpuReset()
{
	std::memset(&cpuRegs, 0, sizeof(cpuRegs));
	std::memset(&fpuRegs, 0, sizeof(fpuRegs));
	std::memset(&tlb, 0, sizeof(tlb));
	cachedTlbs.count = 0;

	cpuRegs.pc				= 0xbfc00000; //set pc reg to stack
	cpuRegs.CP0.n.Config	= 0x440;
	cpuRegs.CP0.n.Status.val= 0x70400004; //0x10900000 <-- wrong; // COP0 enabled | BEV = 1 | TS = 1
	cpuRegs.CP0.n.Random	= 47;   // top TLB index; counts down towards Wired
	cpuRegs.CP0.n.PRid		= 0x00002e20; // PRevID = Revision ID, same as R5900
	fpuRegs.fprc[0]			= 0x00002e30; // fpu Revision..
	fpuRegs.fprc[31]		= 0x01000001; // fpu Status/Control

	cpuRegs.nextEventCycle = cpuRegs.cycle + 4;
	EEsCycle = 0;
	EEoCycle = cpuRegs.cycle;

	psxReset();
	pgifInit();

	extern void Deci2Reset();		// lazy, no good header for it yet.
	Deci2Reset();

	AllowParams1 = !VMManager::Internal::IsFastBootInProgress();
	AllowParams2 = !VMManager::Internal::IsFastBootInProgress();
	ParamsRead = false;

	g_eeloadMain = 0;
	g_eeloadExec = 0;
	g_osdsys_str = 0;

	CBreakPoints::ClearSkipFirst();
}

bool eeTlbInvalidMatch = false;   // set by cpuTlbMiss, consumed here

// Every exception except the timer interrupt, so the run-up to a userspace
// fault can be read off directly instead of inferred. TLB misses are the
// normal case and would drown everything else, so they are counted rather
// than printed unless they are at an address that keeps repeating.
static int eeExcBudget = 300;

// Guest syscall trace -- an strace for something we cannot rebuild.
//
// Linux userspace enters the kernel with the syscall instruction, which shows
// up here as excode 8 with the call number in v0. MIPS Linux numbers start at
// 4000, which conveniently separates them from the PS2 BIOS syscalls that use
// this same exception and would otherwise drown the log.
//
// Bounded: bash issues thousands, and the interesting part is the first few
// hundred, up to wherever it goes wrong.
static int eeSyscallBudget = 400;

// The last few user-space faults, so an exception carries its own run-up
// instead of needing to be correlated against a separate trace by hand.
static u32 eeRecentUserFault[8];
static u32 eeRecentUserFaultPC[8];
static int eeRecentUserFaultAt = 0;

__ri void cpuException(u32 code, u32 bd)
{
	bool errLevel2, checkStatus;
	u32 offset = 0;

    cpuRegs.branch = 0;		// Tells the interpreter that an exception occurred during a branch.
	const bool tlbInvalid = eeTlbInvalidMatch;
	eeTlbInvalidMatch = false;

	{
		const u32 excode = (code >> 2) & 0x1f;

		if (excode == 8 && eeSyscallBudget > 0)
		{
			const u32 nr = cpuRegs.GPR.n.v0.UL[0];
			if (nr >= 4000 && nr < 5000)
			{
				eeSyscallBudget--;
				Console.WriteLn("  SYS %4u a0=%08x a1=%08x a2=%08x pc=%08x asid=%02x",
					nr - 4000, cpuRegs.GPR.n.a0.UL[0], cpuRegs.GPR.n.a1.UL[0],
					cpuRegs.GPR.n.a2.UL[0], cpuRegs.pc,
					cpuRegs.CP0.n.EntryHi & 0xff);
			}
		}

		// 0 = interrupt, 2/3 = the TLB misses that are the normal path,
		// 8 = syscall, which the BIOS issues constantly.
		if (excode != 0 && excode != 2 && excode != 3 && excode != 8 && eeExcBudget > 0)
		{
			eeExcBudget--;
			Console.WriteLn("  EXC %u pc=%08x epc=%08x badv=%08x asid=%02x sr=%08x",
				excode, cpuRegs.pc, cpuRegs.CP0.n.EPC, cpuRegs.CP0.n.BadVAddr,
				cpuRegs.CP0.n.EntryHi & 0xff, cpuRegs.CP0.n.Status.val);

			std::string recent;
			for (int i = 0; i < 8; i++)
			{
				const int at = (eeRecentUserFaultAt + i) & 7;
				if (eeRecentUserFault[at])
					recent += StringUtil::StdStringFromFormat("%08x@%08x ",
						eeRecentUserFault[at], eeRecentUserFaultPC[at]);
			}
			Console.WriteLn("      recent user faults: %s", recent.c_str());
		}
	}
	cpuRegs.CP0.n.Cause = code & 0xffff;

	if(cpuRegs.CP0.n.Status.b.ERL == 0)
	{
		//Error Level 0-1
		errLevel2 = false;
		checkStatus = (cpuRegs.CP0.n.Status.b.BEV == 0); //  for TLB/general exceptions

		if (((code & 0x7C) >= 0x8) && ((code & 0x7C) <= 0xC))
			offset = tlbInvalid ? 0x180 : 0x0; // TLB Invalid vs Refill
		else if ((code & 0x7C) == 0x0)
			offset = 0x200; //Interrupt
		else
			offset = 0x180; // Everything else
	}
	else
	{
		//Error Level 2
		errLevel2 = true;
		checkStatus = (cpuRegs.CP0.n.Status.b.DEV == 0); // for perf/debug exceptions

		Console.Error("*PCSX2* FIX ME: Level 2 cpuException");
		if ((code & 0x38000) <= 0x8000 )
		{
			//Reset / NMI
			cpuRegs.pc = 0xBFC00000;
			Console.Warning("Reset request");
			cpuUpdateOperationMode();
			return;
		}
		else if ((code & 0x38000) == 0x10000)
			offset = 0x80; //Performance Counter
		else if ((code & 0x38000) == 0x18000)
			offset = 0x100; //Debug
		else
			Console.Error("Unknown Level 2 Exception!! Cause %x", code);
	}

	if (cpuRegs.CP0.n.Status.b.EXL == 0)
	{
		cpuRegs.CP0.n.Status.b.EXL = 1;
		if (bd)
		{
			// Rate-limited: an exception in a delay slot is rare for games but
			// routine for a demand-paged OS, where it floods the log.
			static int s_bdSpamStop = 0;
			if (s_bdSpamStop++ < 50 || IsDevBuild)
				Console.Warning("branch delay!! (%d)", s_bdSpamStop);
			cpuRegs.CP0.n.EPC = cpuRegs.pc - 4;
			cpuRegs.CP0.n.Cause |= 0x80000000;
		}
		else
		{
			cpuRegs.CP0.n.EPC = cpuRegs.pc;
			cpuRegs.CP0.n.Cause &= ~0x80000000;
		}
	}
	else
	{
		offset = 0x180; //Override the cause
		if (errLevel2) Console.Warning("cpuException: Status.EXL = 1 cause %x", code);
	}

	if (checkStatus)
		cpuRegs.pc = 0x80000000 + offset;
	else
		cpuRegs.pc = 0xBFC00200 + offset;

	cpuUpdateOperationMode();
}

// ---- kernelreloaded instrumentation ---------------------------------------
// PS2 Linux writes its console to the GS framebuffer, so once the kernel is
// running nothing it says reaches this log. When it stops there is no way from
// the outside to tell a livelock from a halt from a spin inside a driver.
//
// This prints the whole EE state roughly once a second: pc with disassembly,
// every GPR, the CP0 control registers with the exception cause decoded, the
// stack, and all 48 TLB entries.
//
// It translates addresses itself rather than going through the vtlb, for two
// reasons: a dump must never fault or recurse into the exception path it is
// trying to describe, and going through the vtlb would show the mapping PCSX2
// thinks it has rather than the one the guest's TLB actually describes -- the
// difference between the two being exactly what is under suspicion.
static u32 eeTlbMissCount = 0;
static u32 eeTlbMissLast = 0;
static u32 eeTlbMissPrev = 0;
static u32 eeLastDump = 0;

static void cpuStateDump(bool force, const char* label);
// Forward declaration: real definition is further down this file (next to
// cpuStateDump's own use of it), needed here too by
// kernelreloadedCheckTraceRange() below, which is deliberately placed next
// to kernelreloadedCheckBreakpoint() rather than down by cpuStateDump().
static bool eeRead32(u32 va, u32* out);

// kernelreloaded: on-demand breakpoint dumps, driven by -kload-break on the
// CLI (QtHost.cpp). Deliberately separate from the stock CBreakPoints/
// isBreakpointNeeded machinery in Interpreter.cpp, which is compiled out
// entirely unless PCSX2_DEVBUILD or EXTRA_DEBUG is defined -- this needs to
// work in the same plain release build build.sh produces. Checked every
// instruction from execI(); the vector is empty unless -kload-break was
// passed, so the cost when unused is one size()==0 check per instruction.
static std::vector<u32> s_kernelreloadedBreakpoints;

void kernelreloadedSetBreakpoints(const char* addrListCsv)
{
	s_kernelreloadedBreakpoints.clear();
	if (!addrListCsv)
		return;

	std::string list(addrListCsv);
	size_t pos = 0;
	while (pos < list.size())
	{
		const size_t comma = list.find(',', pos);
		const std::string tok = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
		if (!tok.empty())
		{
			const u32 addr = static_cast<u32>(std::strtoul(tok.c_str(), nullptr, 16));
			s_kernelreloadedBreakpoints.push_back(addr);
			Console.WriteLn("kernelreloaded: breakpoint armed at 0x%08x", addr);
		}
		if (comma == std::string::npos)
			break;
		pos = comma + 1;
	}
}

void kernelreloadedCheckBreakpoint(u32 pc)
{
	if (s_kernelreloadedBreakpoints.empty())
		return;
	for (const u32 addr : s_kernelreloadedBreakpoints)
	{
		if (addr == pc)
		{
			cpuStateDump(true, "BREAKPOINT");
			return;
		}
	}
}

// kernelreloaded: -kload-trace-range implementation. See the declaration in
// R5900.h for the full rationale. Deliberately one line per instruction
// (disassembly only, not a full cpuStateDump()) -- a full register dump per
// hit is what made -kload-break runs against a tight loop painfully slow
// earlier in this same investigation (real time roughly quadrupled with
// several breakpoints active simultaneously); a range trace is meant to
// cover hundreds of instructions in sequence, where that cost would compound
// hundreds of times over for comparatively little extra information (the
// PC trajectory itself is the point, not the full GPR file at every step).
static u32 s_kernelreloadedTraceStart = 0;
static u32 s_kernelreloadedTraceEnd = 0;
static int s_kernelreloadedTraceRemaining = 0;
// kernelreloaded: trigger gate, added after the first real run of this
// feature showed why it's needed -- a 60,000-instruction budget exhausted
// by cycle 1.77s of a boot whose actual moment of interest was ~24s in.
// The BIOS ROM's low-address range isn't reserved for the one call path
// under investigation; it's the BIOS's own general-purpose library code
// (printf-style formatting, string/memory helpers), hit constantly for
// completely mundane reasons throughout boot. Tracing "every time PC is in
// this range" from process start makes the budget mean "however many
// instructions of unrelated BIOS busywork happen to run first" -- useless.
// A trigger address makes the trace inert (parsed, armed, but logging
// nothing and spending no budget) until that one specific PC is hit once,
// then active for the next maxHits instructions in range. 0 means no
// trigger -- active immediately, the original (now rarely useful) behavior.
static u32 s_kernelreloadedTraceTrigger = 0;
static bool s_kernelreloadedTraceTriggered = false;

void kernelreloadedSetTraceRange(const char* spec)
{
	s_kernelreloadedTraceStart = 0;
	s_kernelreloadedTraceEnd = 0;
	s_kernelreloadedTraceRemaining = 0;
	s_kernelreloadedTraceTrigger = 0;
	s_kernelreloadedTraceTriggered = false;
	if (!spec)
		return;

	std::string s(spec);
	const size_t c1 = s.find(',');
	if (c1 == std::string::npos)
	{
		Console.WriteLn("kernelreloaded: -kload-trace-range needs start,end[,maxHits[,triggerPC]]");
		return;
	}
	const size_t c2 = s.find(',', c1 + 1);
	const std::string startTok = s.substr(0, c1);
	const std::string endTok = s.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);

	s_kernelreloadedTraceStart = static_cast<u32>(std::strtoul(startTok.c_str(), nullptr, 16));
	s_kernelreloadedTraceEnd = static_cast<u32>(std::strtoul(endTok.c_str(), nullptr, 16));
	s_kernelreloadedTraceRemaining = 300;
	if (c2 != std::string::npos)
	{
		const size_t c3 = s.find(',', c2 + 1);
		const std::string maxTok = s.substr(c2 + 1, c3 == std::string::npos ? std::string::npos : c3 - c2 - 1);
		s_kernelreloadedTraceRemaining = std::atoi(maxTok.c_str());
		if (c3 != std::string::npos)
		{
			const std::string triggerTok = s.substr(c3 + 1);
			s_kernelreloadedTraceTrigger = static_cast<u32>(std::strtoul(triggerTok.c_str(), nullptr, 16));
		}
	}
	// No trigger given means "active from the start" -- same as marking it
	// already triggered.
	s_kernelreloadedTraceTriggered = (s_kernelreloadedTraceTrigger == 0);

	Console.WriteLn("kernelreloaded: trace range armed 0x%08x-0x%08x, %d instructions%s",
		s_kernelreloadedTraceStart, s_kernelreloadedTraceEnd, s_kernelreloadedTraceRemaining,
		s_kernelreloadedTraceTrigger ? StringUtil::StdStringFromFormat(
			", gated on trigger 0x%08x", s_kernelreloadedTraceTrigger).c_str() : "");
}

void kernelreloadedCheckTraceRange(u32 pc)
{
	if (s_kernelreloadedTraceRemaining <= 0)
		return;

	if (!s_kernelreloadedTraceTriggered)
	{
		if (pc != s_kernelreloadedTraceTrigger)
			return;
		s_kernelreloadedTraceTriggered = true;
		Console.WriteLn("  RANGE trigger 0x%08x hit -- trace active", pc);
	}

	if (pc < s_kernelreloadedTraceStart || pc >= s_kernelreloadedTraceEnd)
		return;

	u32 word;
	std::string text;
	if (eeRead32(pc, &word))
		R5900::disR5900Fasm(text, word, pc, false);
	else
		text = "<unmapped>";

	s_kernelreloadedTraceRemaining--;
	Console.WriteLn("  RANGE %08x: %s%s", pc, text.c_str(),
		s_kernelreloadedTraceRemaining == 0 ? " (trace range exhausted)" : "");
}


// Bounded because the failure runs at ~10M faults a second: enough events to
// replay the sequence by hand, then silence.
int eeTraceBudget = 400;

// Separate from eeTraceBudget on purpose -- NEARNULL is meant to survive that
// budget being spent on unrelated spam, since it was written for a rare,
// one-off signature. But "rare" assumed the fault resolves; a genuine crash
// loop (a bad jump that re-faults on the same near-null address every cycle)
// hits this at the same unbounded rate eeTraceBudget exists to guard against,
// and did: 11M lines in a few seconds, enough to back up the Qt log window's
// queued cross-thread appends and read as the whole process hanging. Bounded
// the same way, just kept in its own pool.
int eeNearNullBudget = 200;

static const char* eeSeg(u32 a)
{
	if (a >= 0xE0000000) return "kseg3";
	if (a >= 0xC0000000) return "kseg2";
	if (a >= 0xA0000000) return "kseg1";
	if (a >= 0x80000000) return "kseg0";
	return "useg";
}

// Walk the guest TLB by hand. Returns false when the address is genuinely
// unmapped -- which is the interesting answer, not an error.
static bool eeTranslate(u32 va, u32* pa)
{
	if (va >= 0x80000000 && va < 0xC0000000)
	{
		*pa = va & 0x1FFFFFFF;
		return true;
	}

	for (int i = 0; i < 48; i++)
	{
		const u32 pageSize = (tlb[i].Mask() + 1) << 12;
		const u32 base = tlb[i].VPN2();

		if (va < base || va >= base + pageSize * 2)
			continue;
		if (!tlb[i].isGlobal() && (tlb[i].EntryHi.ASID != (cpuRegs.CP0.n.EntryHi & 0xff)))
			continue;

		const bool odd = (va - base) >= pageSize;
		const EntryLo_t& lo = odd ? tlb[i].EntryLo1 : tlb[i].EntryLo0;
		if (!lo.V)
			return false;

		*pa = (odd ? tlb[i].PFN1() : tlb[i].PFN0()) + ((va - base) & (pageSize - 1));
		return true;
	}
	return false;
}

// MIPS raises two different exceptions for a failed translation, and they do
// NOT share a vector:
//
//   no entry matches the address        -> TLB Refill,  vector 0x000
//   an entry matches but has V=0        -> TLB Invalid, vector 0x180
//
// PCSX2 only ever knew "mapped or not" -- MapTLB skips V=0 entries entirely --
// so it sent both to 0x000. That breaks demand paging, which is built on the
// difference: the refill handler installs the (invalid) PTE it finds, and the
// retry is meant to trap to 0x180 and reach do_page_fault, which allocates the
// page. Sent back to 0x000 instead, the handler reinstalls the same zero PTE
// forever. Games never notice because they map everything up front and never
// demand-page.
static bool eeTlbMatches(u32 va)
{
	for (int i = 0; i < 48; i++)
	{
		const u32 pageSize = (tlb[i].Mask() + 1) << 12;
		const u32 base = tlb[i].VPN2();

		if (va < base || va >= base + pageSize * 2)
			continue;
		if (!tlb[i].isGlobal() && (tlb[i].EntryHi.ASID != (cpuRegs.CP0.n.EntryHi & 0xff)))
			continue;

		// A matching entry that were valid would not have missed, so reaching
		// here means it matched and was invalid.
		return true;
	}
	return false;
}

static bool eeRead32(u32 va, u32* out)
{
	u32 pa;
	if (!eeTranslate(va, &pa))
		return false;
	if (!eeMem || pa + 4 > Ps2MemSize::ExposedRam)
		return false;
	*out = *reinterpret_cast<u32*>(&eeMem->Main[pa]);
	return true;
}

static const char* eeExcName(u32 code)
{
	switch (code)
	{
		case 0:  return "Int";
		case 1:  return "TLBMod";
		case 2:  return "TLBL";
		case 3:  return "TLBS";
		case 4:  return "AdEL";
		case 5:  return "AdES";
		case 6:  return "IBE";
		case 7:  return "DBE";
		case 8:  return "Sys";
		case 9:  return "Bp";
		case 10: return "RI";
		case 11: return "CpU";
		case 12: return "Ov";
		case 13: return "Tr";
		default: return "?";
	}
}

// force=true / label!=nullptr is the on-demand path from
// kernelreloadedCheckBreakpoint() -- skips the interval gate entirely and
// labels the dump so it's distinguishable in the log from the periodic one.
static void cpuStateDump(bool force = false, const char* label = "EE state")
{
	// cpuRegs.cycle is declared u64 but wraps at 32 bits in practice, so a
	// "next due" absolute deadline past 2^32 is never reached and the dump
	// stops for good. Compare a 32-bit delta instead, which wraps with it.
	static const u32 interval = 1500 * 1000 * 1000;

	if (!force)
	{
		if (static_cast<u32>(static_cast<u32>(cpuRegs.cycle) - eeLastDump) < interval)
			return;
		eeLastDump = static_cast<u32>(cpuRegs.cycle);
	}

	const u32 pc = cpuRegs.pc;
	const u32 sp = cpuRegs.GPR.r[29].UL[0];
	const u32 exc = (cpuRegs.CP0.n.Cause >> 2) & 0x1f;

	Console.WriteLn("======== %s @ cycle %llu ========", label, cpuRegs.cycle);
	Console.WriteLn("pc   =%08x (%s)   sp=%08x (%s)   ra=%08x",
		pc, eeSeg(pc), sp, eeSeg(sp), cpuRegs.GPR.r[31].UL[0]);

	// Instructions at pc, so a spin loop is identifiable on sight.
	for (int i = 0; i < 6; i++)
	{
		u32 word;
		if (!eeRead32(pc + i * 4, &word))
		{
			Console.WriteLn("  %08x: <unmapped>", pc + i * 4);
			continue;
		}
		std::string text;
		R5900::disR5900Fasm(text, word, pc + i * 4, false);
		Console.WriteLn("  %08x: %08x  %s", pc + i * 4, word, text.c_str());
	}

	Console.WriteLn("sr   =%08x  cause=%08x exc=%u(%s)  epc=%08x  errepc=%08x",
		cpuRegs.CP0.n.Status.val, cpuRegs.CP0.n.Cause, exc, eeExcName(exc),
		cpuRegs.CP0.n.EPC, cpuRegs.CP0.n.ErrorEPC);
	Console.WriteLn("badv =%08x  entryhi=%08x (asid=%02x)  index=%08x random=%u wired=%u",
		cpuRegs.CP0.n.BadVAddr, cpuRegs.CP0.n.EntryHi, cpuRegs.CP0.n.EntryHi & 0xff,
		cpuRegs.CP0.n.Index, cpuRegs.CP0.n.Random, cpuRegs.CP0.n.Wired);
	Console.WriteLn("lo0  =%08x  lo1=%08x  pagemask=%08x  context=%08x  count=%08x compare=%08x",
		cpuRegs.CP0.n.EntryLo0, cpuRegs.CP0.n.EntryLo1, cpuRegs.CP0.n.PageMask,
		cpuRegs.CP0.n.Context, cpuRegs.CP0.n.Count, cpuRegs.CP0.n.Compare);
	Console.WriteLn("tlb misses since last dump: %u  last=%08x prev=%08x",
		eeTlbMissCount, eeTlbMissLast, eeTlbMissPrev);

	for (int i = 0; i < 32; i += 4)
	{
		Console.WriteLn("  %-4s=%08x %-4s=%08x %-4s=%08x %-4s=%08x",
			R5900::GPR_REG[i + 0], cpuRegs.GPR.r[i + 0].UL[0],
			R5900::GPR_REG[i + 1], cpuRegs.GPR.r[i + 1].UL[0],
			R5900::GPR_REG[i + 2], cpuRegs.GPR.r[i + 2].UL[0],
			R5900::GPR_REG[i + 3], cpuRegs.GPR.r[i + 3].UL[0]);
	}

	std::string line;
	for (int i = 0; i < 8; i++)
	{
		u32 word;
		line += StringUtil::StdStringFromFormat(
			eeRead32(sp + i * 4, &word) ? "%08x " : "-------- ", word);
	}
	Console.WriteLn("stack@sp: %s", line.c_str());

	int valid = 0;
	for (int i = 0; i < 48; i++)
	{
		if (!tlb[i].EntryLo0.V && !tlb[i].EntryLo1.V)
			continue;
		valid++;
		Console.WriteLn("  tlb[%2d] vpn2=%08x size=%6uk pfn0=%08x%s pfn1=%08x%s asid=%02x%s",
			i, tlb[i].VPN2(), ((tlb[i].Mask() + 1) << 12) / 1024,
			tlb[i].PFN0(), tlb[i].EntryLo0.V ? "" : "(inv)",
			tlb[i].PFN1(), tlb[i].EntryLo1.V ? "" : "(inv)",
			tlb[i].EntryHi.ASID, tlb[i].isGlobal() ? " G" : "");
	}
	Console.WriteLn("tlb entries in use: %d/48", valid);

	eeTlbMissCount = 0;
}

void cpuTlbMiss(u32 addr, u32 bd, u32 excode)
{
	eeTlbMissCount++;
	eeTlbMissPrev = eeTlbMissLast;
	eeTlbMissLast = addr;

	// Must be decided before EntryHi is rewritten below, since the ASID in it
	// is what the match is against.
	eeTlbInvalidMatch = eeTlbMatches(addr);

	if (addr < 0x80000000)
	{
		eeRecentUserFault[eeRecentUserFaultAt] = addr;
		eeRecentUserFaultPC[eeRecentUserFaultAt] = cpuRegs.pc;
		eeRecentUserFaultAt = (eeRecentUserFaultAt + 1) & 7;
	}

	if (eeTraceBudget > 0)
	{
		eeTraceBudget--;
		Console.WriteLn("  TLBMISS addr=%08x epc/pc=%08x exc=%u hi=%08x ctx=%08x",
			addr, cpuRegs.pc, excode, cpuRegs.CP0.n.EntryHi, cpuRegs.CP0.n.Context);
	}

	// Avoid too much spamming on the interpreter
	if (Cpu != &intCpu || IsDebugBuild) {
		Console.Error("cpuTlbMiss pc:%x, cycl:%x, addr: %x, status=%x, code=%x",
				cpuRegs.pc, cpuRegs.cycle, addr, cpuRegs.CP0.n.Status.val, excode);
	}

	// A fault on a near-null address is never legitimate here, and it is the
	// signature of the bash crash: exccode 3 (TLB store) reported at a pc
	// whose instruction is a syscall, which performs no store. Logged outside
	// eeTraceBudget so it survives that budget being spent elsewhere, but
	// still bounded by its own budget -- see eeNearNullBudget.
	if (addr < 0x10000 && eeNearNullBudget > 0)
	{
		eeNearNullBudget--;
		Console.Error("  NEARNULL addr=%08x pc=%08x excode=%u branch=%u opcode=%08x "
			"delayop=%08x epc=%08x cause=%08x",
			addr, cpuRegs.pc, excode, (u32)cpuRegs.branch, cpuRegs.code,
			cpuRegs.branch ? 1u : 0u,
			cpuRegs.CP0.n.EPC, cpuRegs.CP0.n.Cause);
		if (eeNearNullBudget == 0)
		{
			// This is the generic signal, not a PS2Linux-specific one: the
			// comment above already treats any near-null fault as never
			// legitimate, and the same address has now faulted 200 times
			// running, so this is not a fluke to let ride. Left running, it
			// just spins at whatever fault rate the interpreter can sustain
			// forever with no way for the guest to recover and nothing
			// visible changing on screen -- indistinguishable from PCSX2
			// itself having hung, which is exactly the failure mode a
			// contained emulator (the QEMU comparison this was raised
			// against) should not have.
			//
			// Ask to shut down was the original plan here (allow_confirm=true,
			// so the user could decline and inspect the hung state) --
			// confirmed 2026-08-22, via the same-shaped check in
			// R5900OpcodeImpl.cpp's _unknownOpcodeCrashLoopCheck, that this
			// does not work: Host::RequestVMShutdown() needs the CPU thread
			// to reach a cooperative stop check, and this call site never
			// advances pc, so the interpreter calls right back in here next
			// cycle. The confirm dialog appears, but clicking it then hangs
			// waiting on a checkpoint that will never come.
			// AlertUserAndExit() (common/HostSys.cpp) shows a synchronous
			// native alert directly on this thread -- no Qt, no cooperative
			// check -- then unconditionally exits.
			Console.Error("  NEARNULL: further messages suppressed (faulting in a tight loop)");
			// kernelreloaded (2026-08-23): was Host::ReportErrorAsync() +
			// Host::RequestVMShutdown(true, ...). Confirmed live that this
			// hangs -- the Qt "are you sure?" dialog appears and responds to
			// clicks, but nothing ever tears the VM down, because that needs
			// this same CPU thread (the one stuck in the fault loop that got
			// us here) to reach a cooperative stop check it will never reach.
			// Needed a real force-quit to recover. AlertUserAndExit() is
			// deliberately independent of Qt/VMManager/this thread's own
			// future cooperation -- same guarantee Assertions.cpp's Windows
			// MessageBoxA()+TerminateProcess() branch already has. Two-arg
			// form used over origin/whiterhino's one-arg convention (same
			// bug, independently fixed there 2026-08-22) so the native
			// alert's title names the actual condition instead of a generic
			// "Fatal error" -- see common/HostSys.cpp for both overloads.
			AlertUserAndExit("Guest CPU crash loop detected",
				fmt::format("Faulting repeatedly on near-null address 0x{:08x} at pc 0x{:08x}.", addr, cpuRegs.pc));
		}
	}

	cpuRegs.CP0.n.BadVAddr = addr;
	cpuRegs.CP0.n.Context &= 0xFF80000F;
	cpuRegs.CP0.n.Context |= (addr >> 9) & 0x007FFFF0;
	cpuRegs.CP0.n.EntryHi = (addr & 0xFFFFE000) | (cpuRegs.CP0.n.EntryHi & 0x1FFF);

	cpuRegs.pc -= 4;
	cpuException(excode, bd);
}

// Store to a page whose EntryLo.D is clear. Vectors to 0x180 like any other
// non-refill exception, which cpuException already does for this code.
void cpuTlbModified(u32 addr, u32 bd) {
	cpuTlbMiss(addr, bd, EXC_CODE(1));
}

void cpuTlbMissR(u32 addr, u32 bd) {
	cpuTlbMiss(addr, bd, EXC_CODE_TLBL);
}

void cpuTlbMissW(u32 addr, u32 bd) {
	cpuTlbMiss(addr, bd, EXC_CODE_TLBS);
}

// sets a branch test to occur some time from an arbitrary starting point.
__fi void cpuSetNextEvent( u32 startCycle, s32 delta )
{
	// typecast the conditional to signed so that things don't blow up
	// if startCycle is greater than our next branch cycle.

	if( (int)(cpuRegs.nextEventCycle - startCycle) > delta )
	{
		cpuRegs.nextEventCycle = startCycle + delta;
	}
}

// sets a branch to occur some time from the current cycle
__fi void cpuSetNextEventDelta( s32 delta )
{
	cpuSetNextEvent( cpuRegs.cycle, delta );
}

__fi int cpuGetCycles(int interrupt)
{
	if(interrupt == VU_MTVU_BUSY && (!THREAD_VU1 || INSTANT_VU1))
		return 1;
	else
	{
		const int cycles = (cpuRegs.sCycle[interrupt] + cpuRegs.eCycle[interrupt]) - cpuRegs.cycle;
		return std::max(1, cycles);
	}

}

// tests the cpu cycle against the given start and delta values.
// Returns true if the delta time has passed.
__fi int cpuTestCycle( u32 startCycle, s32 delta )
{
	// typecast the conditional to signed so that things don't explode
	// if the startCycle is ahead of our current cpu cycle.

	return (int)(cpuRegs.cycle - startCycle) >= delta;
}

// tells the EE to run the branch test the next time it gets a chance.
__fi void cpuSetEvent()
{
	cpuRegs.nextEventCycle = cpuRegs.cycle;
}

__fi void cpuClearInt( uint i )
{
	pxAssume( i < 32 );
	cpuRegs.interrupt &= ~(1 << i);
	cpuRegs.dmastall &= ~(1 << i);
}

static __fi void TESTINT( u8 n, void (*callback)() )
{
	if( !(cpuRegs.interrupt & (1 << n)) ) return;

	if(CHECK_INSTANTDMAHACK || cpuTestCycle( cpuRegs.sCycle[n], cpuRegs.eCycle[n] ) )
	{
		cpuClearInt( n );
		callback();
	}
	else
		cpuSetNextEvent( cpuRegs.sCycle[n], cpuRegs.eCycle[n] );
}

// [TODO] move this function to Dmac.cpp, and remove most of the DMAC-related headers from
// being included into R5900.cpp.
static __fi bool _cpuTestInterrupts()
{

	if (!dmacRegs.ctrl.DMAE || (psHu8(DMAC_ENABLER+2) & 1))
	{
		//Console.Write("DMAC Disabled or suspended");
		return false;
	}

	eeRunInterruptScan = INT_RUNNING;

	while (eeRunInterruptScan == INT_RUNNING)
	{
		/* These are 'pcsx2 interrupts', they handle asynchronous stuff
		   that depends on the cycle timings */
		TESTINT(VU_MTVU_BUSY, MTVUInterrupt);
		TESTINT(DMAC_VIF1, vif1Interrupt);
		TESTINT(DMAC_GIF, gifInterrupt);
		TESTINT(DMAC_SIF0, EEsif0Interrupt);
		TESTINT(DMAC_SIF1, EEsif1Interrupt);
		// Profile-guided Optimization (sorta)
		// The following ints are rarely called.  Encasing them in a conditional
		// as follows helps speed up most games.

		if (cpuRegs.interrupt & ((1 << DMAC_VIF0) | (1 << DMAC_FROM_IPU) | (1 << DMAC_TO_IPU)
			| (1 << DMAC_FROM_SPR) | (1 << DMAC_TO_SPR) | (1 << DMAC_MFIFO_VIF) | (1 << DMAC_MFIFO_GIF)
			| (1 << VIF_VU0_FINISH) | (1 << VIF_VU1_FINISH) | (1 << IPU_PROCESS)))
		{
			TESTINT(DMAC_VIF0, vif0Interrupt);

			TESTINT(DMAC_FROM_IPU, ipu0Interrupt);
			TESTINT(DMAC_TO_IPU, ipu1Interrupt);
			TESTINT(IPU_PROCESS, ipuCMDProcess);

			TESTINT(DMAC_FROM_SPR, SPRFROMinterrupt);
			TESTINT(DMAC_TO_SPR, SPRTOinterrupt);

			TESTINT(DMAC_MFIFO_VIF, vifMFIFOInterrupt);
			TESTINT(DMAC_MFIFO_GIF, gifMFIFOInterrupt);

			TESTINT(VIF_VU0_FINISH, vif0VUFinish);
			TESTINT(VIF_VU1_FINISH, vif1VUFinish);
		}

		if (eeRunInterruptScan == INT_REQ_LOOP)
			eeRunInterruptScan = INT_RUNNING;
		else
			break;
	}

	eeRunInterruptScan = INT_NOT_RUNNING;

	if ((cpuRegs.interrupt & 0x1FFFF) & ~cpuRegs.dmastall)
		return true;
	else
		return false;
}

static __fi void _cpuTestTIMR()
{
	cpuRegs.CP0.n.Count += cpuRegs.cycle - cpuRegs.lastCOP0Cycle;
	cpuRegs.lastCOP0Cycle = cpuRegs.cycle;

	// fixme: this looks like a hack to make up for the fact that the TIMR
	// doesn't yet have a proper mechanism for setting itself up on a nextEventCycle.
	// A proper fix would schedule the TIMR to trigger at a specific cycle anytime
	// the Count or Compare registers are modified.

	if ( (cpuRegs.CP0.n.Status.val & 0x8000) &&
		cpuRegs.CP0.n.Count >= cpuRegs.CP0.n.Compare && cpuRegs.CP0.n.Count < cpuRegs.CP0.n.Compare+1000 )
	{
		Console.WriteLn( Color_Magenta, "timr intr: %x, %x", cpuRegs.CP0.n.Count, cpuRegs.CP0.n.Compare);
		cpuException(0x808000, cpuRegs.branch);
	}
}

static __fi void _cpuTestPERF()
{
	// Perfs are updated when read by games (COP0's MFC0/MTC0 instructions), so we need
	// only update them at semi-regular intervals to keep cpuRegs.cycle from wrapping
	// around twice on us btween updates.  Hence this function is called from the cpu's
	// Counters update.

	COP0_UpdatePCCR();
}

// Checks the COP0.Status for exception enablings.
// Exception handling for certain modes is *not* currently supported, this function filters
// them out.  Exceptions while the exception handler is active (EIE), or exceptions of any
// level other than 0 are ignored here.

static bool cpuIntsEnabled(int Interrupt)
{
	bool IntType = !!(cpuRegs.CP0.n.Status.val & Interrupt); //Choose either INTC or DMAC, depending on what called it

	return IntType && cpuRegs.CP0.n.Status.b.EIE && cpuRegs.CP0.n.Status.b.IE &&
		!cpuRegs.CP0.n.Status.b.EXL && (cpuRegs.CP0.n.Status.b.ERL == 0);
}

// Shared portion of the branch test, called from both the Interpreter
// and the recompiler.  (moved here to help alleviate redundant code)
__fi void _cpuEventTest_Shared()
{
	eeEventTestIsActive = true;
	cpuStateDump();
	cpuRegs.nextEventCycle = cpuRegs.cycle + eeWaitCycles;
	cpuRegs.lastEventCycle = cpuRegs.cycle;
	// ---- INTC / DMAC (CPU-level Exceptions) -----------------
	// Done first because exceptions raised during event tests need to be postponed a few
	// cycles (fixes Grandia II [PAL], which does a spin loop on a vsync and expects to
	// be able to read the value before the exception handler clears it).

	uint mask = intcInterrupt() | dmacInterrupt();
	if (cpuIntsEnabled(mask))
		cpuException(mask, cpuRegs.branch);

	// ---- IOP -------------
	// * It's important to run a iopEventTest before calling ExecuteBlock. This
	//   is because the IOP does not always perform branch tests before returning
	//   (during the prev branch) and also so it can act on the state the EE has
	//   given it before executing any code.
	//
	// * The IOP cannot always be run.  If we run IOP code every time through the
	//   cpuEventTest, the IOP generally starts to run way ahead of the EE.

	// It's also important to sync up the IOP before updating the timers, since gates will depend on starting/stopping in the right place!
	EEsCycle += cpuRegs.cycle - EEoCycle;
	EEoCycle = cpuRegs.cycle;

	if (EEsCycle > 0)
		iopEventAction = true;

	if (iopEventAction)
	{
		//if( EEsCycle < -450 )
		//	Console.WriteLn( " IOP ahead by: %d cycles", -EEsCycle );

		EEsCycle = psxCpu->ExecuteBlock(EEsCycle);

		iopEventAction = false;
	}

	iopEventTest();

	if (cpuTestCycle(nextStartCounter, nextDeltaCounter))
	{
		rcntUpdate();
		_cpuTestPERF();
	}

	_cpuTestTIMR();

	// ---- Interrupts -------------
	// These are basically just DMAC-related events, which also piggy-back the same bits as
	// the PS2's own DMA channel IRQs and IRQ Masks.

	if (cpuRegs.interrupt)
	{
		// This is a BIOS hack because the coding in the BIOS is terrible but the bug is masked by Data Cache
		// where a DMA buffer is overwritten without waiting for the transfer to end, which causes the fonts to get all messed up
		// so to fix it, we run all the DMA's instantly when in the BIOS.
		// Only use the lower 17 bits of the cpuRegs.interrupt as the upper bits are for VU0/1 sync which can't be done in a tight loop
		if (CHECK_INSTANTDMAHACK && dmacRegs.ctrl.DMAE && !(psHu8(DMAC_ENABLER + 2) & 1) && (cpuRegs.interrupt & 0x1FFFF))
		{
			while ((cpuRegs.interrupt & 0x1FFFF) && _cpuTestInterrupts())
				;
		}
		else
			_cpuTestInterrupts();
	}

	// ---- VU Sync -------------
	// We're in a EventTest.  All dynarec registers are flushed
	// so there is no need to freeze registers here.
	CpuVU0->ExecuteBlock();
	CpuVU1->ExecuteBlock();

	// ---- Schedule Next Event Test --------------
	const float mutiplier = static_cast<float>(PS2CLK) / static_cast<float>(PSXCLK);
	const int nextIopEventDeta = ((psxRegs.iopNextEventCycle - psxRegs.cycle) * mutiplier);
	// 8 or more cycles behind and there's an event scheduled
	if (EEsCycle >= nextIopEventDeta)
	{
		// EE's running way ahead of the IOP still, so we should branch quickly to give the
		// IOP extra timeslices in short order.

		cpuSetNextEventDelta(48);
		//Console.Warning( "EE ahead of the IOP -- Rapid Event!  %d", EEsCycle );
	}
	else
	{
		// Otherwise IOP is caught up/not doing anything so we can wait for the next event.
		cpuSetNextEventDelta(((psxRegs.iopNextEventCycle - psxRegs.cycle) * mutiplier) - EEsCycle);
	}

	// Apply vsync and other counter nextCycles
	cpuSetNextEvent(nextStartCounter, nextDeltaCounter);

	eeEventTestIsActive = false;
}

__ri void cpuTestINTCInts()
{
	// Check the COP0's Status register for general interrupt disables, and the 0x400
	// bit (which is INTC master toggle).
	if (!cpuIntsEnabled(0x400))
		return;

	if ((psHu32(INTC_STAT) & psHu32(INTC_MASK)) == 0)
		return;

	cpuSetNextEventDelta(4);
	if (eeEventTestIsActive && (psxRegs.iopCycleEE > 0))
	{
		psxRegs.iopBreak += psxRegs.iopCycleEE; // record the number of cycles the IOP didn't run.
		psxRegs.iopCycleEE = 0;
	}
}

__fi void cpuTestDMACInts()
{
	// Check the COP0's Status register for general interrupt disables, and the 0x800
	// bit (which is the DMAC master toggle).
	if (!cpuIntsEnabled(0x800))
		return;

	if (((psHu16(0xe012) & psHu16(0xe010)) == 0) &&
		((psHu16(0xe010) & 0x8000) == 0))
		return;

	cpuSetNextEventDelta(4);
	if (eeEventTestIsActive && (psxRegs.iopCycleEE > 0))
	{
		psxRegs.iopBreak += psxRegs.iopCycleEE; // record the number of cycles the IOP didn't run.
		psxRegs.iopCycleEE = 0;
	}
}

__fi void cpuTestTIMRInts()
{
	if ((cpuRegs.CP0.n.Status.val & 0x10007) == 0x10001)
	{
		_cpuTestPERF();
		_cpuTestTIMR();
	}
}

__fi void cpuTestHwInts()
{
	cpuTestINTCInts();
	cpuTestDMACInts();
	cpuTestTIMRInts();
}

__fi void CPU_SET_DMASTALL(EE_EventType n, bool set)
{
	if (set)
		cpuRegs.dmastall |= 1 << n;
	else
		cpuRegs.dmastall &= ~(1 << n);
}

__fi void CPU_INT( EE_EventType n, s32 ecycle)
{
	// If it's retunning too quick, just rerun the DMA, there's no point in running the EE for < 4 cycles.
	// This causes a huge uplift in performance for ONI FMV's.
	if (ecycle < 4 && !(cpuRegs.dmastall & (1 << n)) && eeRunInterruptScan != INT_NOT_RUNNING)
	{
		eeRunInterruptScan = INT_REQ_LOOP;
		cpuRegs.interrupt |= 1 << n;
		cpuRegs.sCycle[n] = cpuRegs.cycle;
		cpuRegs.eCycle[n] = 0;
		return;
	}

	// EE events happen 8 cycles in the future instead of whatever was requested.
	// This can be used on games with PATH3 masking issues for example, or when
	// some FMV look bad.
	if (CHECK_EETIMINGHACK && n < VIF_VU0_FINISH)
		ecycle = 8;

	cpuRegs.interrupt |= 1 << n;
	cpuRegs.sCycle[n] = cpuRegs.cycle;
	cpuRegs.eCycle[n] = ecycle;

	// Interrupt is happening soon: make sure both EE and IOP are aware.

	if (ecycle <= 28 && psxRegs.iopCycleEE > 0)
	{
		// If running in the IOP, force it to break immediately into the EE.
		// the EE's branch test is due to run.

		psxRegs.iopBreak += psxRegs.iopCycleEE; // record the number of cycles the IOP didn't run.
		psxRegs.iopCycleEE = 0;
	}

	cpuSetNextEventDelta(cpuRegs.eCycle[n]);
}

// Count arguments, save their starting locations, and replace the space separators with null terminators so they're separate strings
int ParseArgumentString(u32 arg_block)
{
	if (!arg_block)
		return 0;

	int argc = 0;
	bool wasSpace = true; // status of last char. scanned
	int args_len = strlen((char *)PSM(arg_block));
	for (int i = 0; i < args_len; i++)
	{
		char curchar = *(char *)PSM(arg_block + i);
		if (curchar == '\0')
			break; // should never reach this

		bool isSpace = (curchar == ' ');
		if (isSpace)
			memset(PSM(arg_block + i), 0, 1);
		else if (wasSpace) // then we're at a new arg
		{
			if (argc < kMaxArgs)
			{
				g_argPtrs[argc] = arg_block + i;
				argc++;
			}
			else
			{
				Console.WriteLn("ParseArgumentString: Discarded additional arguments beyond the maximum of %d.", kMaxArgs);
				break;
			}
		}
		wasSpace = isSpace;
	}
#if DEBUG_LAUNCHARG
	// Check our args block
	Console.WriteLn("ParseArgumentString: Saving these strings:");
	for (int a = 0; a < argc; a++)
		Console.WriteLn("%p -> '%s'.", g_argPtrs[a], (char *)PSM(g_argPtrs[a]));
#endif
	return argc;
}

// Called from recompilers; define is mandatory.
void eeloadHook()
{
	// PS2 Linux direct boot.
	//
	// Taken here rather than straight after cpuReset() because the SBIOS
	// needs the IOP's published SIF RPC address, which only exists once the
	// BIOS has booted and brought the IOP up. By the time EELOAD is called
	// that has happened, and nothing of what EELOAD would have launched is
	// wanted -- DirectBoot() sets pc to the kernel entry, so returning here
	// redirects execution into it.
	if (PS2Linux::IsDirectBootRequested())
	{
		static bool s_direct_boot_done = false;
		if (!s_direct_boot_done)
		{
			s_direct_boot_done = true;
			std::string db_error;
			if (PS2Linux::DirectBoot(PS2Linux::GetRequestedBootParams(), &db_error))
				return;

			// A partial failure here already means SBIOS/kernel/initrd bytes
			// were written into memory the real EELOAD/OSDSYS path still
			// expects to own, so letting the interpreter fall through and
			// keep executing real EELOAD code on top of that runs on
			// corrupted state -- observed in practice as an immediate
			// near-null jump, spinning forever. That specific case is now
			// also caught by the NEARNULL crash-loop check above, but this
			// fires first and shouldn't assume every corrupted-state outcome
			// takes that exact shape. Report the failure and exit rather
			// than letting that happen.
			//
			// kernelreloaded (2026-08-23): was Host::ReportErrorAsync() +
			// Host::RequestVMShutdown(true, ...), same as the NEARNULL guard
			// above -- same confirmed hang (Host::RequestVMShutdown() needs
			// the CPU thread to reach a cooperative stop check, which a
			// spinning interpreter never does; confirmed 2026-08-22
			// elsewhere in this file and in R5900OpcodeImpl.cpp), same fix.
			// AlertUserAndExit() (common/HostSys.cpp) shows a synchronous
			// native alert directly on this thread instead, then
			// unconditionally exits. It doesn't offer a "decline and
			// inspect" choice the way the old allow_confirm=true shutdown
			// dialog did, but that choice never actually worked here (the
			// CPU thread it needs is the one already corrupted/wedged), so
			// removing it costs nothing real. Two-arg form used over
			// origin/whiterhino's one-arg convention (same bug,
			// independently fixed there 2026-08-22) so the native alert's
			// title names the actual condition instead of a generic "Fatal
			// error" -- see common/HostSys.cpp for both overloads.
			Console.Error(fmt::format("PS2 Linux direct boot failed: {}", db_error));
			AlertUserAndExit("PS2 Linux direct boot failed", db_error);
			return;
		}
	}

	std::string elfname;
	int argc = cpuRegs.GPR.n.a0.SD[0];
	if (argc) // calls to EELOAD *after* the first one during the startup process will come here
	{
#if DEBUG_LAUNCHARG
		Console.WriteLn("eeloadHook: EELOAD was called with %d arguments according to $a0 and %d according to vargs block:",
			argc, memRead32(cpuRegs.GPR.n.a1.UD[0] - 4));
		for (int a = 0; a < argc; a++)
			Console.WriteLn("argv[%d]: %p -> %p -> '%s'", a, cpuRegs.GPR.n.a1.UL[0] + (a * 4),
				memRead32(cpuRegs.GPR.n.a1.UD[0] + (a * 4)), (char *)PSM(memRead32(cpuRegs.GPR.n.a1.UD[0] + (a * 4))));
#endif
		if (argc > 1)
			elfname = (char*)PSM(memRead32(cpuRegs.GPR.n.a1.UD[0] + 4)); // argv[1] in OSDSYS's invocation "EELOAD <game ELF>"

		// This code fires if the user chooses "full boot". First the Sony Computer Entertainment screen appears. This is the result
		// of an EELOAD call that does not want to accept launch arguments (but we patch it to do so in eeloadHook2() in fast boot
		// mode). Then EELOAD is called with the argument "rom0:PS2LOGO". At this point, we do not need any additional tricks
		// because EELOAD is now ready to accept launch arguments. So in full-boot mode, we simply wait for PS2LOGO to be called,
		// then we add the desired launch arguments. PS2LOGO passes those on to the game itself as it calls EELOAD a third time.
		if (!EmuConfig.CurrentGameArgs.empty() && elfname == "rom0:PS2LOGO")
		{
			const char *argString = EmuConfig.CurrentGameArgs.c_str();
			Console.WriteLn("eeloadHook: Supplying launch argument(s) '%s' to module '%s'...", argString, elfname.c_str());

			// Join all arguments by space characters so they can be processed as one string by ParseArgumentString(), then add the
			// user's launch arguments onto the end
			u32 arg_ptr = 0;
			int arg_len = 0;
			for (int a = 0; a < argc; a++)
			{
				arg_ptr = memRead32(cpuRegs.GPR.n.a1.UD[0] + (a * 4));
				arg_len = strlen((char *)PSM(arg_ptr));
				memset(PSM(arg_ptr + arg_len), 0x20, 1);
			}
			strcpy((char *)PSM(arg_ptr + arg_len + 1), EmuConfig.CurrentGameArgs.c_str());
			u32 first_arg_ptr = memRead32(cpuRegs.GPR.n.a1.UD[0]);
#if DEBUG_LAUNCHARG
			Console.WriteLn("eeloadHook: arg block is '%s'.", (char *)PSM(first_arg_ptr));
#endif
			argc = ParseArgumentString(first_arg_ptr);

			// Write pointer to next slot in $a1
			for (int a = 0; a < argc; a++)
				memWrite32(cpuRegs.GPR.n.a1.UD[0] + (a * 4), g_argPtrs[a]);
			cpuRegs.GPR.n.a0.SD[0] = argc;
#if DEBUG_LAUNCHARG
			// Check our work
			Console.WriteLn("eeloadHook: New arguments are:");
			for (int a = 0; a < argc; a++)
				Console.WriteLn("argv[%d]: %p -> '%s'", a, memRead32(cpuRegs.GPR.n.a1.UD[0] + (a * 4)),
				(char *)PSM(memRead32(cpuRegs.GPR.n.a1.UD[0] + (a * 4))));
#endif
		}
		// else it's presumed that the invocation is "EELOAD <game ELF> <<launch args>>", coming from PS2LOGO, and we needn't do
		// anything more
	}
#if DEBUG_LAUNCHARG
	// This code fires in full/fast boot mode when EELOAD is called the first/only time. When EELOAD is not given any arguments,
	// it calls rom0:OSDSYS by default, which displays the Sony Computer Entertainment screen. OSDSYS then calls "EELOAD
	// rom0:PS2LOGO" and we end up above.
	else
		Console.WriteLn("eeloadHook: EELOAD was called with no arguments.");
#endif

	// If "fast boot" was chosen, then on EELOAD's first call we won't yet know what the game's ELF is. Find the name and write it
	// into EELOAD's memory.
	if (VMManager::Internal::IsFastBootInProgress() && elfname.empty())
	{
		const std::string& elf_override = VMManager::Internal::GetELFOverride();
		if (!elf_override.empty())
		{
			elfname = fmt::format("host:{}", elf_override);
		}
		else
		{
			CDVDDiscType disc_type;
			std::string disc_elf;
			cdvdGetDiscInfo(nullptr, &disc_elf, nullptr, nullptr, &disc_type);
			if (disc_type == CDVDDiscType::PS2Disc)
			{
				// only allow fast boot for PS2 games
				elfname = std::move(disc_elf);
			}
			else
			{
				Console.Warning(fmt::format("Not allowing fast boot for non-PS2 ELF {}", disc_elf));
			}
		}

		// When fast-booting, we insert the game's ELF name into EELOAD so that the game is called instead of the default call of
		// "rom0:OSDSYS"; any launch arguments supplied by the user will be inserted into EELOAD later by eeloadHook2()
		if (!elfname.empty())
		{
			// Find and save location of default/fallback call "rom0:OSDSYS"; to be used later by eeloadHook2()
			for (g_osdsys_str = EELOAD_START; g_osdsys_str < EELOAD_START + EELOAD_SIZE; g_osdsys_str += 8) // strings are 64-bit aligned
			{
				if (!strcmp((char*)PSM(g_osdsys_str), "rom0:OSDSYS"))
				{
					// Overwrite OSDSYS with game's ELF name
					strcpy((char*)PSM(g_osdsys_str), elfname.c_str());
				}
			}
		}
		else
		{
			// Stop fast forwarding if we're doing that for boot.
			VMManager::Internal::DisableFastBoot();
			AllowParams1 = true;
			AllowParams2 = true;
		}
	}

	VMManager::Internal::ELFLoadingOnCPUThread(std::move(elfname));

	if (CHECK_EXTRAMEM)
	{
		// Map extra memory.
		vtlb_VMap(Ps2MemSize::MainRam, Ps2MemSize::MainRam, Ps2MemSize::ExtraRam);

		// Map RAM mirrors for extra memory.
		vtlb_VMap(0x20000000 | Ps2MemSize::MainRam, Ps2MemSize::MainRam, Ps2MemSize::ExtraRam);
		vtlb_VMap(0x30000000 | Ps2MemSize::MainRam, Ps2MemSize::MainRam, Ps2MemSize::ExtraRam);
	}
}

// Called from recompilers; define is mandatory.
// Only called if g_SkipBiosHack is true
void eeloadHook2()
{
	if (EmuConfig.CurrentGameArgs.empty())
		return;

	if (!g_osdsys_str)
	{
		Console.WriteLn("eeloadHook2: Called before \"rom0:OSDSYS\" was found by eeloadHook()!");
		return;
	}

	const char *argString = EmuConfig.CurrentGameArgs.c_str();
	Console.WriteLn("eeloadHook2: Supplying launch argument(s) '%s' to ELF '%s'.", argString, (char *)PSM(g_osdsys_str));

	// Add args string after game's ELF name that was written over "rom0:OSDSYS" by eeloadHook(). In between the ELF name and args
	// string we insert a space character so that ParseArgumentString() has one continuous string to process.
	int game_len = strlen((char *)PSM(g_osdsys_str));
	memset(PSM(g_osdsys_str + game_len), 0x20, 1);
	strcpy((char *)PSM(g_osdsys_str + game_len + 1), EmuConfig.CurrentGameArgs.c_str());
#if DEBUG_LAUNCHARG
	Console.WriteLn("eeloadHook2: arg block is '%s'.", (char *)PSM(g_osdsys_str));
#endif
	int argc = ParseArgumentString(g_osdsys_str);

	// Back up 4 bytes from start of args block for every arg + 4 bytes for start of argv pointer block, write pointers
	uptr block_start = g_osdsys_str - (argc * 4);
	for (int a = 0; a < argc; a++)
	{
#if DEBUG_LAUNCHARG
		Console.WriteLn("eeloadHook2: Writing address %p to location %p.", g_argPtrs[a], block_start + (a * 4));
#endif
		memWrite32(block_start + (a * 4), g_argPtrs[a]);
	}

	// Save argc and argv as incoming arguments for EELOAD function which calls ExecPS2()
#if DEBUG_LAUNCHARG
	Console.WriteLn("eeloadHook2: Saving %d and %p in $a0 and $a1.", argc, block_start);
#endif
	cpuRegs.GPR.n.a0.SD[0] = argc;
	cpuRegs.GPR.n.a1.UD[0] = block_start;
}

inline bool isBranchOrJump(u32 addr)
{
	u32 op = memRead32(addr);
	const OPCODE& opcode = GetInstruction(op);

	// Return false for eret & syscall as they are branch type in pcsx2 debugging tools,
	// but shouldn't have delay slot in isBreakpointNeeded/isMemcheckNeeded.
	if ((opcode.flags == (IS_BRANCH | BRANCHTYPE_SYSCALL)) || (opcode.flags == (IS_BRANCH | BRANCHTYPE_ERET)))
		return false;

	return (opcode.flags & IS_BRANCH) != 0;
}

// The next two functions return 0 if no breakpoint is needed,
// 1 if it's needed on the current pc, 2 if it's needed in the delay slot
// 3 if needed in both

int isBreakpointNeeded(u32 addr)
{
	int bpFlags = 0;
	if (CBreakPoints::IsAddressBreakPoint(BREAKPOINT_EE, addr))
		bpFlags += 1;

	// there may be a breakpoint in the delay slot
	if (isBranchOrJump(addr) && CBreakPoints::IsAddressBreakPoint(BREAKPOINT_EE, addr+4))
		bpFlags += 2;

	return bpFlags;
}

int isMemcheckNeeded(u32 pc)
{
	if (CBreakPoints::GetNumMemchecks() == 0)
		return 0;

	u32 addr = pc;
	if (isBranchOrJump(addr))
		addr += 4;

	u32 op = memRead32(addr);
	const OPCODE& opcode = GetInstruction(op);

	if (opcode.flags & IS_MEMORY)
		return addr == pc ? 1 : 2;

	return 0;
}
