// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "R3000A.h"
#include "Common.h"

#include <cstdlib>
#include <string>
#include <vector>

#include "common/StringUtil.h"
#include "DebugTools/Debug.h"

#include "SIO/Sio0.h"
#include "Sif.h"
#include "DebugTools/Breakpoints.h"
#include "R5900OpcodeTables.h"
#include "IopCounters.h"
#include "IopBios.h"
#include "IopHw.h"
#include "IopDma.h"
#include "IopMem.h"
#include "CDVD/Ps1CD.h"
#include "CDVD/CDVD.h"

using namespace R3000A;

R3000Acpu *psxCpu;

// used for constant propagation
u32 g_psxConstRegs[32];
u32 g_psxHasConstReg, g_psxFlushedConstReg;

// Used to signal to the EE when important actions that need IOP-attention have
// happened (hsyncs, vsyncs, IOP exceptions, etc).  IOP runs code whenever this
// is true, even if it's already running ahead a bit.
bool iopEventAction = false;

static constexpr uint iopWaitCycles = 384; // Keep inline with EE wait cycle max.

bool iopEventTestIsActive = false;

alignas(16) psxRegisters psxRegs;

void psxReset()
{
	std::memset(&psxRegs, 0, sizeof(psxRegs));

	psxRegs.pc = 0xbfc00000; // Start in bootstrap
	psxRegs.CP0.n.Status = 0x00400000; // BEV = 1
	psxRegs.CP0.n.PRid   = 0x0000001f; // PRevID = Revision ID, same as the IOP R3000A

	psxRegs.iopBreak = 0;
	psxRegs.iopCycleEE = -1;
	psxRegs.iopCycleEECarry = 0;
	psxRegs.iopNextEventCycle = psxRegs.cycle + 4;

	psxHwReset();
	PSXCLK = 36864000;
	ioman::reset();
	psxBiosReset();
}

void psxShutdown() {
	//psxCpu->Shutdown();
}

// kernelreloaded: IOP-side breakpoint mechanism, direct counterpart to
// R5900.cpp's own kernelreloadedSetBreakpoints/kernelreloadedCheckBreakpoint/
// cpuStateDump. Built 2026-08-23 chasing a NetBSD boot hang whose EE-side
// trail dead-ended at a jalr through a BIOS-provided function pointer that
// resolved to a plausible, correctly-set address (0x80001020, inside the
// real retail BIOS ROM's own reserved region) -- meaning the fault was no
// longer in any code this project's own source controls, and the next real
// question (does the IOP ever answer whatever SIF-RPC request the EE-side
// BIOS is making) has no answer without IOP-side visibility, which nothing
// in this codebase provided. See netbsd's porting-notes.md for the full
// investigation this was built to continue.
static std::vector<u32> s_kernelreloadedIopBreakpoints;

void kernelreloadedSetIopBreakpoints(const char* addrListCsv)
{
	s_kernelreloadedIopBreakpoints.clear();
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
			s_kernelreloadedIopBreakpoints.push_back(addr);
			Console.WriteLn("kernelreloaded: IOP breakpoint armed at 0x%08x", addr);
		}
		if (comma == std::string::npos)
			break;
		pos = comma + 1;
	}
}

// Mirrors R5900.cpp's cpuStateDump(), scaled to what the IOP actually has:
// no TLB (the R3000A is unmapped -- kuseg/kseg0/kseg1 only, no MMU), so no
// TLB entry table at the end. GPR names, COP0 layout (Status/Cause/EPC/
// BadVAddr), and the "disassemble a few instructions at pc" idea all carry
// over directly from the EE version.
static void psxStateDump(const char* label)
{
	const u32 pc = psxRegs.pc;

	Console.WriteLn("======== IOP %s @ cycle %u ========", label, psxRegs.cycle);
	Console.WriteLn("pc   =%08x   sp=%08x   ra=%08x",
		pc, psxRegs.GPR.n.sp, psxRegs.GPR.n.ra);

	for (int i = 0; i < 6; i++)
	{
		const u32 addr = pc + i * 4;
		const u32 word = iopMemRead32(addr);
		Console.WriteLn("  %08x: %08x  %s", addr, word, disR3000AF(word, addr));
	}

	Console.WriteLn("sr   =%08x  cause=%08x  epc=%08x  badv=%08x",
		psxRegs.CP0.n.Status, psxRegs.CP0.n.Cause, psxRegs.CP0.n.EPC, psxRegs.CP0.n.BadVAddr);

	Console.WriteLn("  zero=%08x at  =%08x v0  =%08x v1  =%08x",
		psxRegs.GPR.n.r0, psxRegs.GPR.n.at, psxRegs.GPR.n.v0, psxRegs.GPR.n.v1);
	Console.WriteLn("  a0  =%08x a1  =%08x a2  =%08x a3  =%08x",
		psxRegs.GPR.n.a0, psxRegs.GPR.n.a1, psxRegs.GPR.n.a2, psxRegs.GPR.n.a3);
	Console.WriteLn("  t0  =%08x t1  =%08x t2  =%08x t3  =%08x",
		psxRegs.GPR.n.t0, psxRegs.GPR.n.t1, psxRegs.GPR.n.t2, psxRegs.GPR.n.t3);
	Console.WriteLn("  t4  =%08x t5  =%08x t6  =%08x t7  =%08x",
		psxRegs.GPR.n.t4, psxRegs.GPR.n.t5, psxRegs.GPR.n.t6, psxRegs.GPR.n.t7);
	Console.WriteLn("  s0  =%08x s1  =%08x s2  =%08x s3  =%08x",
		psxRegs.GPR.n.s0, psxRegs.GPR.n.s1, psxRegs.GPR.n.s2, psxRegs.GPR.n.s3);
	Console.WriteLn("  s4  =%08x s5  =%08x s6  =%08x s7  =%08x",
		psxRegs.GPR.n.s4, psxRegs.GPR.n.s5, psxRegs.GPR.n.s6, psxRegs.GPR.n.s7);
	Console.WriteLn("  t8  =%08x t9  =%08x k0  =%08x k1  =%08x",
		psxRegs.GPR.n.t8, psxRegs.GPR.n.t9, psxRegs.GPR.n.k0, psxRegs.GPR.n.k1);
	Console.WriteLn("  gp  =%08x sp  =%08x s8  =%08x ra  =%08x  hi=%08x lo=%08x",
		psxRegs.GPR.n.gp, psxRegs.GPR.n.sp, psxRegs.GPR.n.s8, psxRegs.GPR.n.ra,
		psxRegs.GPR.n.hi, psxRegs.GPR.n.lo);

	std::string line;
	for (int i = 0; i < 8; i++)
		line += StringUtil::StdStringFromFormat("%08x ", iopMemRead32(psxRegs.GPR.n.sp + i * 4));
	Console.WriteLn("stack@sp: %s", line.c_str());
}

void kernelreloadedCheckIopBreakpoint(u32 pc)
{
	if (s_kernelreloadedIopBreakpoints.empty())
		return;
	for (const u32 addr : s_kernelreloadedIopBreakpoints)
	{
		if (addr == pc)
		{
			psxStateDump("BREAKPOINT");
			return;
		}
	}
}

// Bounded the same way eeExcBudget is in R5900.cpp: every exception is the
// normal path for some IOP code (syscalls in particular fire constantly),
// so this needs a ceiling or it drowns everything else, but a real IOP-side
// fault -- the RPC-call hang this was built for could plausibly be one --
// should still show up with a real trail instead of silently vanishing into
// budget exhaustion with no marker that it happened at all.
static int psxExcBudget = 300;

void psxException(u32 code, u32 bd)
{
//	PSXCPU_LOG("psxException %x: %x, %x", code, psxHu32(0x1070), psxHu32(0x1074));
	//Console.WriteLn("!! psxException %x: %x, %x", code, psxHu32(0x1070), psxHu32(0x1074));
	if (psxExcBudget > 0)
	{
		psxExcBudget--;
		const u32 excode = (code >> 2) & 0x1f;
		// 8 = syscall, the IOP kernel's own normal path -- excluded the same
		// way R5900.cpp's cpuException() excludes TLB refill/syscall, or this
		// budget burns out in the first few dozen cycles of any real boot.
		if (excode != 8)
		{
			Console.WriteLn("  IOP EXC %u pc=%08x cause=%08x sr=%08x%s",
				excode, psxRegs.pc, code, psxRegs.CP0.n.Status, psxExcBudget == 0 ? " (further IOP EXC suppressed)" : "");
		}
	}
	// Set the Cause
	psxRegs.CP0.n.Cause &= ~0x7f;
	psxRegs.CP0.n.Cause |= code;

	// Set the EPC & PC
	if (bd)
	{
		PSXCPU_LOG("bd set");
		psxRegs.CP0.n.Cause|= 0x80000000;
		psxRegs.CP0.n.EPC = (psxRegs.pc - 4);
	}
	else
		psxRegs.CP0.n.EPC = (psxRegs.pc);

	if (psxRegs.CP0.n.Status & 0x400000)
		psxRegs.pc = 0xbfc00180;
	else
		psxRegs.pc = 0x80000080;

	// Set the Status
	psxRegs.CP0.n.Status = (psxRegs.CP0.n.Status &~0x3f) |
						  ((psxRegs.CP0.n.Status & 0xf) << 2);

	/*if ((((PSXMu32(psxRegs.CP0.n.EPC) >> 24) & 0xfe) == 0x4a)) {
		// "hokuto no ken" / "Crash Bandicot 2" ... fix
		PSXMu32(psxRegs.CP0.n.EPC)&= ~0x02000000;
	}*/

	/*if (psxRegs.CP0.n.Cause == 0x400 && (!(psxHu32(0x1450) & 0x8))) {
		hwIntcIrq(INTC_SBUS);
	}*/
}

__fi void psxSetNextBranch( u32 startCycle, s32 delta )
{
	// typecast the conditional to signed so that things don't blow up
	// if startCycle is greater than our next branch cycle.

	if( (int)(psxRegs.iopNextEventCycle - startCycle) > delta )
		psxRegs.iopNextEventCycle = startCycle + delta;
}

__fi void psxSetNextBranchDelta( s32 delta )
{
	psxSetNextBranch( psxRegs.cycle, delta );
}

__fi int psxTestCycle( u32 startCycle, s32 delta )
{
	// typecast the conditional to signed so that things don't explode
	// if the startCycle is ahead of our current cpu cycle.

	return (int)(psxRegs.cycle - startCycle) >= delta;
}

__fi int psxRemainingCycles(IopEventId n)
{
	if (psxRegs.interrupt & (1 << n))
		return ((psxRegs.cycle - psxRegs.sCycle[n]) + psxRegs.eCycle[n]);
	else
		return 0;
}

__fi void PSX_INT( IopEventId n, s32 ecycle )
{
	// 19 is CDVD read int, it's supposed to be high.
	//if (ecycle > 8192 && n != 19)
	//	DevCon.Warning( "IOP cycles high: %d, n %d", ecycle, n );

	psxRegs.interrupt |= 1 << n;

	psxRegs.sCycle[n] = psxRegs.cycle;
	psxRegs.eCycle[n] = ecycle;

	psxSetNextBranchDelta(ecycle);
	const float mutiplier = static_cast<float>(PS2CLK) / static_cast<float>(PSXCLK);
	const s32 iopDelta = (psxRegs.iopNextEventCycle - psxRegs.cycle) * mutiplier;

	if (psxRegs.iopCycleEE < iopDelta)
	{
		// The EE called this int, so inform it to branch as needed:
		
		cpuSetNextEventDelta(iopDelta - psxRegs.iopCycleEE);
	}
}

static __fi void IopTestEvent( IopEventId n, void (*callback)() )
{
	if( !(psxRegs.interrupt & (1 << n)) ) return;

	if( psxTestCycle( psxRegs.sCycle[n], psxRegs.eCycle[n] ) )
	{
		psxRegs.interrupt &= ~(1 << n);
		callback();
	}
	else
		psxSetNextBranch( psxRegs.sCycle[n], psxRegs.eCycle[n] );
}

static __fi void Sio0TestEvent(IopEventId n)
{
	if (!(psxRegs.interrupt & (1 << n)))
	{
		return;
	}

	if (psxTestCycle(psxRegs.sCycle[n], psxRegs.eCycle[n]))
	{
		psxRegs.interrupt &= ~(1 << n);
		g_Sio0.Interrupt(Sio0Interrupt::TEST_EVENT);
	}
	else
	{
		psxSetNextBranch(psxRegs.sCycle[n], psxRegs.eCycle[n]);
	}
}

static __fi void _psxTestInterrupts()
{
	IopTestEvent(IopEvt_SIF0,		sif0Interrupt);	// SIF0
	IopTestEvent(IopEvt_SIF1,		sif1Interrupt);	// SIF1
	IopTestEvent(IopEvt_SIF2,		sif2Interrupt);	// SIF2
	Sio0TestEvent(IopEvt_SIO);
	IopTestEvent(IopEvt_CdvdSectorReady, cdvdSectorReady);
	IopTestEvent(IopEvt_CdvdRead,	cdvdReadInterrupt);

	// Profile-guided Optimization (sorta)
	// The following ints are rarely called.  Encasing them in a conditional
	// as follows helps speed up most games.

	if( psxRegs.interrupt & ((1 << IopEvt_Cdvd) | (1 << IopEvt_Dma11) | (1 << IopEvt_Dma12)
		| (1 << IopEvt_Cdrom) | (1 << IopEvt_CdromRead) | (1 << IopEvt_DEV9) | (1 << IopEvt_USB)))
	{
		IopTestEvent(IopEvt_Cdvd,		cdvdActionInterrupt);
		IopTestEvent(IopEvt_Dma11,		psxDMA11Interrupt);	// SIO2
		IopTestEvent(IopEvt_Dma12,		psxDMA12Interrupt);	// SIO2
		IopTestEvent(IopEvt_Cdrom,		cdrInterrupt);
		IopTestEvent(IopEvt_CdromRead,	cdrReadInterrupt);
		IopTestEvent(IopEvt_DEV9,		dev9Interrupt);
		IopTestEvent(IopEvt_USB,		usbInterrupt);
	}
}

__ri void iopEventTest()
{
	psxRegs.iopNextEventCycle = psxRegs.cycle + iopWaitCycles;

	if (psxTestCycle(psxNextStartCounter, psxNextDeltaCounter))
	{
		psxRcntUpdate();
		iopEventAction = true;
	}
	else
	{
		// start the next branch at the next counter event by default
		// the interrupt code below will assign nearer branches if needed.
		if (psxNextDeltaCounter < static_cast<s32>(psxRegs.iopNextEventCycle - psxNextStartCounter))
			psxRegs.iopNextEventCycle = psxNextStartCounter + psxNextDeltaCounter;
	}

	if (psxRegs.interrupt)
	{
		iopEventTestIsActive = true;
		_psxTestInterrupts();
		iopEventTestIsActive = false;
	}

	if ((psxHu32(HW_ICTRL) != 0) && ((psxHu32(HW_ISTAT) & psxHu32(HW_IMASK)) != 0))
	{
		if ((psxRegs.CP0.n.Status & 0xFE01) >= 0x401)
		{
			PSXCPU_LOG("Interrupt: %x  %x", psxHu32(HW_ISTAT), psxHu32(HW_IMASK));
			psxException(0, 0);
			iopEventAction = true;
		}
	}
}

void iopTestIntc()
{
	if( psxHu32(HW_ICTRL) == 0 ) return;
	if( (psxHu32(HW_ISTAT) & psxHu32(HW_IMASK)) == 0 ) return;

	if( !eeEventTestIsActive )
	{
		// An iop exception has occurred while the EE is running code.
		// Inform the EE to branch so the IOP can handle it promptly:

		cpuSetNextEventDelta( 16 );
		iopEventAction = true;
		//Console.Error( "** IOP Needs an EE EventText, kthx **  %d", iopCycleEE );

		// Note: No need to set the iop's branch delta here, since the EE
		// will run an IOP branch test regardless.
	}
	else if ( !iopEventTestIsActive )
		psxSetNextBranchDelta( 2 );
}

inline bool psxIsBranchOrJump(u32 addr)
{
	u32 op = iopMemRead32(addr);
	const R5900::OPCODE& opcode = R5900::GetInstruction(op);

	return (opcode.flags & IS_BRANCH) != 0;
}

// The next two functions return 0 if no breakpoint is needed,
// 1 if it's needed on the current pc, 2 if it's needed in the delay slot
// 3 if needed in both

int psxIsBreakpointNeeded(u32 addr)
{
	int bpFlags = 0;
	if (CBreakPoints::IsAddressBreakPoint(BREAKPOINT_IOP, addr))
		bpFlags += 1;

	// there may be a breakpoint in the delay slot
	if (psxIsBranchOrJump(addr) && CBreakPoints::IsAddressBreakPoint(BREAKPOINT_IOP, addr + 4))
		bpFlags += 2;

	return bpFlags;
}

int psxIsMemcheckNeeded(u32 pc)
{
	if (CBreakPoints::GetNumMemchecks() == 0)
		return 0;

	u32 addr = pc;
	if (psxIsBranchOrJump(addr))
		addr += 4;

	u32 op = iopMemRead32(addr);
	const R5900::OPCODE& opcode = R5900::GetInstruction(op);

	if (opcode.flags & IS_MEMORY)
		return addr == pc ? 1 : 2;

	return 0;
}
