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
	const size_t numCacheLines = sizeBytes / cacheLineBytes;

	std::vector<uint> bitflips;
	std::bitset<512> bitset;

	// check for bitflips in each cache line
	for (int cl = 0; cl < numCacheLines; cl++) {
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

std::vector<uint> detectSpecificBitflips(const char *data, size_t sizeBytes,
					 const std::bitset<512> &expectedPattern,
					 const std::vector<uint> &bitflipLocations)
{
	const int bitsPerByte = 8;
	const int cacheLineBytes = 64;
	const int cacheLineBits = cacheLineBytes * bitsPerByte;
	const size_t numCacheLines = sizeBytes / cacheLineBytes;

	std::vector<uint> bitflips;
	std::bitset<512> bitset;

	// Only check specified locations
	for (auto bitflipLocation : bitflipLocations) {
		uint cl = bitflipLocation / cacheLineBits;
		uint offset = bitflipLocation % cacheLineBits;

		// Ensure that the location is within the bounds
		if (cl < numCacheLines) {
			bitset.reset();

			// Reconstruct the bitset for the specific cache line
			for (int i = 0; i < cacheLineBytes; i++) {
				auto nextByte = data[cl * cacheLineBytes + i];
				bitset |= (nextByte << i * bitsPerByte);
			}

			auto errorMask = bitset ^ expectedPattern;

			// Check if there's an error at the specified offset
			if (errorMask.test(offset)) {
				bitflips.push_back(bitflipLocation);
			}
		}
	}

	return bitflips;
}
