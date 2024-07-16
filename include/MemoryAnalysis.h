#pragma once

#include <vector>
#include <cstdlib>
#include <bitset>

/**
 * @brief Detects bitflips in the read data by comparing it against the input data pattern.
 *
 * @param data Pointer to the read data.
 * @param expectedPattern The expected data pattern.
 * @return A vector of positions where bitflips were detected.
 */
std::vector<uint> detectBitflips(const char *data, size_t sizeBytes,
				 const std::bitset<512> &expectedPattern);

std::vector<uint> detectSpecificBitflips(const char *data, size_t sizeBytes,
					 const std::bitset<512> &expectedPattern,
					 const std::vector<uint> &bitflipLocations);
