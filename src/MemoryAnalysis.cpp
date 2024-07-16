#include "MemoryAnalysis.h"

#include <vector>
#include <cstdlib>
#include <bitset>

std::vector<uint> detectBitflips(const char *data, size_t sizeBytes,
				 const std::bitset<512> &expectedPattern)
{
	std::bitset<512> read_data_bitset;
	std::vector<uint> bitflips;

	const int bitsPerByte = 8;
	const int cacheLineBytes = 64;
	const int cacheLineBits = cacheLineBytes * bitsPerByte;

	// check for bitflips in each cache line
	for (int cl = 0; cl < sizeBytes / cacheLineBytes; cl++) {
		read_data_bitset.reset();

		for (int i = 0; i < cacheLineBytes; i++) {
			auto tmp_bitset = data[cl * cacheLineBytes + i];
			read_data_bitset |= (tmp_bitset << i * bitsPerByte);
		}

		auto error_mask = read_data_bitset ^ expectedPattern;
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
