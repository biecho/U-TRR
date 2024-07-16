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
