#include "MemoryAnalysis.h"

#include <vector>
#include <cstdlib>
#include <bitset>

std::vector<uint> detectBitflips(const char *data, size_t sizeBytes,
				 const std::bitset<512> &expectedPattern)
{
	const int bitsPerByte = 8;
	const int cacheLineBytes = 64;
	const int cacheLineBits = cacheLineBytes * bitsPerByte;

	std::bitset<512> bitset;
	std::vector<uint> bitflips;

	// check for bitflips in each cache line
	for (int cl = 0; cl < sizeBytes / cacheLineBytes; cl++) {
		bitset.reset();

		for (int i = 0; i < cacheLineBytes; i++) {
			auto nextByte = data[cl * cacheLineBytes + i];
			bitset |= (nextByte << i * bitsPerByte);
		}

		auto error_mask = bitset ^ expectedPattern;
		if (error_mask.any()) {
			for (int i = 0; i < error_mask.size(); i++) {
				if (error_mask.test(i)) {
					bitflips.push_back(cl * cacheLineBits + i);
				}
			}
		}
	}

	return bitflips;
}
