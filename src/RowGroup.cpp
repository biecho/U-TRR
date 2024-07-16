#include <algorithm>
#include "RowGroup.h"

std::vector<RowGroup> selectRowGroupsWithRetTimeConstraint(std::vector<RowGroup> &rowGroups,
							   uint numRowGroups,
							   uint maxAllowedRetTimeDiff)
{
	// Make a copy of rowGroups to avoid side effects
	std::vector<RowGroup> sortedRowGroups = rowGroups;

	std::sort(sortedRowGroups.begin(), sortedRowGroups.end(),
		  [](const RowGroup &a, const RowGroup &b) { return a.ret_ms < b.ret_ms; });

	// Use sliding window to find the subset
	for (size_t i = 0; i <= sortedRowGroups.size() - numRowGroups; i++) {
		uint minRetMs = sortedRowGroups[i].ret_ms;
		uint maxRetMs = sortedRowGroups[i + numRowGroups - 1].ret_ms;

		if (maxRetMs - minRetMs <= maxAllowedRetTimeDiff) {
			return { sortedRowGroups.begin() +
					 static_cast<std::vector<RowGroup>::difference_type>(i),
				 sortedRowGroups.begin() +
					 static_cast<std::vector<RowGroup>::difference_type>(
						 i + numRowGroups) };
		}
	}

	return {};
}

vector<RowGroup> filterForExclusiveRowGroups(const vector<RowGroup> &rowGroups,
					     const vector<RowGroup> &candidateRowGroups)
{
	vector<RowGroup> filteredCandidates;

	// Iterate through each candidate row group to determine exclusivity
	for (const auto &candidate : candidateRowGroups) {
		bool hasCommon = false;

		// Check each existing row group for common rows
		for (const auto &rowGroup : rowGroups) {
			if (rowGroup.hasCommonRowWith(candidate)) {
				hasCommon = true;
				break; // Stop checking as we've found a common row
			}
		}

		// Add the candidate to the result list if no common rows are found
		if (!hasCommon) {
			filteredCandidates.push_back(candidate);
		}
	}

	return filteredCandidates;
}
