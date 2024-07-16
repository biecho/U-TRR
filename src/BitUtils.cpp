#include "BitUtils.h"
#include <random>
#include <iostream>

std::bitset<512> generateDataPattern(uint dataPatternType)
{
	const uint32_t default_data_patterns[] = { 0x0,	       0xFFFFFFFF, 0x00000000, 0x55555555,
						   0xAAAAAAAA, 0xAAAAAAAA, 0x55555555 };

	std::bitset<512> data_pattern;
	switch (dataPatternType) {
	case 0: {
		// Create a Mersenne Twister random number generator
		// seeded from a random device
		std::mt19937 rng(std::random_device{}());
		// Distribution to generate a full range of 32-bit numbers
		std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

		for (int pos = 0; pos < 16; pos++) {
			data_pattern <<= 32;
			uint32_t rand_int = dist(rng); // Generate a random 32-bit number
			data_pattern |= rand_int;
		}
		break;
	}
	case 1:
	case 2: // 1's for victim rows, 0's for aggressor rows
	case 3:
	case 4:
	case 5:
	case 6: {
		for (int pos = 0; pos < 16; pos++) {
			data_pattern <<= 32;
			data_pattern |= default_data_patterns[dataPatternType];
		}

		break;
	}
	default: {
		std::cerr << "\x1b[31m" // ANSI escape code for red text
			  << "ERROR: Undefined input data pattern mode: " << dataPatternType
			  << "\x1b[0m" << std::endl; // ANSI escape code to reset text color
		exit(-1);
	}
	}

	return data_pattern;
}
