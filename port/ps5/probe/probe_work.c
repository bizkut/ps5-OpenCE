/* The probe's deterministic work: integer, double and float arithmetic whose
result the guest (x32) and the host (x86-64) must agree on bit for bit, as
the machines of a network game must (docs/ps5_port_plan.md, D9). Compiled
into both, with -ffp-contract=off. */

#include "probe_abi.h"

uint64_t probe_work(uint32_t seed, uint32_t rounds)
{
	uint64_t hash = 1469598103934665603ull ^ seed;
	double value = 1.0 + seed * 1e-3;
	float single = 0.5f + (float)seed;
	union { double value; uint64_t bits; } double_bits;
	union { float value; uint32_t bits; } float_bits;
	uint32_t round;

	for (round = 0; round < rounds; round++)
	{
		hash = (hash ^ round) * 1099511628211ull;
		value = value * 1.0000001 + (double)(hash & 0xffff) * 1e-9;
		single = single / 1.0001f + (float)(round & 7);
		if (value > 1e6)
			value *= 1e-6;
	}
	double_bits.value = value;
	float_bits.value = single;
	return hash ^ double_bits.bits ^ ((uint64_t)float_bits.bits << 17);
}
