// GP2 kick-shape census (TEMPORARY, env-gated, reverted after the census run).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>

struct Gp2KickCensus
{
	// PACKED fast-path path.type: 0=UNKNOWN 1=ADONLY 2=STQRGBAXYZF2 3=STQRGBAXYZ2
	uint64_t typeCalls[4] = {}, typeRegs[4] = {};
	uint64_t prologueRegs = 0, fallbackRegs = 0, reglistCalls = 0, reglistRegs = 0;
	// combined kicks per prim (0..7)
	uint64_t f2Calls[8] = {}, f2Kicks[8] = {};
	uint64_t x2Calls[8] = {}, x2Kicks[8] = {};
	// single-XYZ kicks per prim/adc
	uint64_t xyzf2[8][2] = {}, xyz2[8][2] = {};
	// REGLIST XYZ kicks per prim/adc
	uint64_t rxyzf2[8][2] = {}, rxyz2[8][2] = {};
	// draws per primclass (index 0..3,7)
	uint64_t draws[8] = {}, drawIdx[8] = {};
};

inline bool gp2KickLog()
{
	static bool on = [] { const char* e = std::getenv("GP2_KICK_LOG"); return e && *e; }();
	return on;
}

inline Gp2KickCensus& gp2Census()
{
	static Gp2KickCensus c;
	return c;
}

inline void gp2CensusDump()
{
	static bool once = false;
	if (once)
		return;
	once = true;
	Gp2KickCensus& c = gp2Census();
	std::fprintf(stderr, "GP2KICK_SUM typeCalls=%llu,%llu,%llu,%llu typeRegs=%llu,%llu,%llu,%llu prologue=%llu fallback=%llu reglist=%llu/%llu\n",
		(unsigned long long)c.typeCalls[0], (unsigned long long)c.typeCalls[1],
		(unsigned long long)c.typeCalls[2], (unsigned long long)c.typeCalls[3],
		(unsigned long long)c.typeRegs[0], (unsigned long long)c.typeRegs[1],
		(unsigned long long)c.typeRegs[2], (unsigned long long)c.typeRegs[3],
		(unsigned long long)c.prologueRegs, (unsigned long long)c.fallbackRegs,
		(unsigned long long)c.reglistCalls, (unsigned long long)c.reglistRegs);
	for (int p = 0; p < 8; p++)
		std::fprintf(stderr, "GP2KICK_PRIM p=%d f2=%llu/%llu x2=%llu/%llu xyzf2=%llu,%llu xyz2=%llu,%llu rxyzf2=%llu,%llu rxyz2=%llu,%llu\n",
			p, (unsigned long long)c.f2Calls[p], (unsigned long long)c.f2Kicks[p],
			(unsigned long long)c.x2Calls[p], (unsigned long long)c.x2Kicks[p],
			(unsigned long long)c.xyzf2[p][0], (unsigned long long)c.xyzf2[p][1],
			(unsigned long long)c.xyz2[p][0], (unsigned long long)c.xyz2[p][1],
			(unsigned long long)c.rxyzf2[p][0], (unsigned long long)c.rxyzf2[p][1],
			(unsigned long long)c.rxyz2[p][0], (unsigned long long)c.rxyz2[p][1]);
	for (int p = 0; p < 8; p++)
		std::fprintf(stderr, "GP2KICK_CLASS c=%d draws=%llu idx=%llu\n",
			p, (unsigned long long)c.draws[p], (unsigned long long)c.drawIdx[p]);
}

inline void gp2CensusArm()
{
	static bool armed = false;
	if (!armed)
	{
		armed = true;
		std::atexit(gp2CensusDump);
	}
}
