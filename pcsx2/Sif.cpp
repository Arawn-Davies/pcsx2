// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#define _PC_	// disables MIPS opcode macros.

#include "R3000A.h"
#include "Common.h"
#include "Sif.h"

// kernelreloaded: SIF0/SIF1 transaction tracing, built 2026-08-23 alongside
// the IOP breakpoint mechanism (R3000A.cpp/.h) chasing the same NetBSD boot
// hang. The existing per-transfer log line in dmaSIF0()/dmaSIF1()
// (SIF_LOG("dmaSIF0/1 %s", ...ch.cmqt_to_str())) already prints exactly the
// right summary -- channel control register, EE-side memory address, quadword
// count, tag address -- but SIF_LOG compiles to nothing outside a
// PCSX2_DEVBUILD/EXTRA_DEBUG build, so it was invisible in the exact release
// build this investigation is running. This makes an unconditional,
// budget-gated copy available in any build.
//
// Deliberately a single shared budget for both channels, not one each: SIF0
// (IOP->EE) and SIF1 (EE->IOP) together are the two halves of one RPC
// request/response conversation, and interleaving them in one numbered
// sequence is what makes "did a response ever come back for this request"
// answerable by reading the log in order, rather than needing to
// interleave two separately-numbered logs by hand.
int kernelreloadedSifTraceBudget = 500;

void kernelreloadedSifTrace(const char* channel, const std::string& desc)
{
	if (kernelreloadedSifTraceBudget <= 0)
		return;
	kernelreloadedSifTraceBudget--;
	Console.WriteLn("  SIF %s %s%s", channel, desc.c_str(),
		kernelreloadedSifTraceBudget == 0 ? " (further SIF trace suppressed)" : "");
}

void sifReset()
{
	std::memset(&sif0, 0, sizeof(sif0));
	std::memset(&sif1, 0, sizeof(sif1));
}

bool SaveStateBase::sifFreeze()
{
	if (!FreezeTag("SIFdma"))
		return false;

	Freeze(sif0);
	Freeze(sif1);
	return IsOkay();
}
