#include <algorithm>
#include "RowGroup.h"
#include <boost/filesystem.hpp>

std::vector<RowGroup> selectRowGroupsWithMaxRetTimeDiff(std::vector<RowGroup> &rowGroups,
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

bool getNextJSONObj(boost::filesystem::ifstream &f_row_groups, string &s_weak)
{
	std::string cur_line;
	std::getline(f_row_groups, cur_line);

	s_weak = "";
	while (cur_line != "}") {
		if (f_row_groups.eof())
			return false;

		s_weak += cur_line;
		std::getline(f_row_groups, cur_line);
	}
	s_weak += cur_line;

	return true;
}

std::vector<RowGroup> parseAllRowGroups(string &rowScoutFile)
{
	boost::filesystem::path rowScoutFilePath(rowScoutFile);
	if (!boost::filesystem::exists(rowScoutFilePath)) {
		//		std::cerr << RED_TXT << "ERROR: RowScout file not found: " <<
		//rowScoutFile
		//			  << NORMAL_TXT << std::endl;
		exit(-1);
	}

	boost::filesystem::ifstream rowScoutFileStream;
	rowScoutFileStream.open(rowScoutFilePath);

	vector<RowGroup> rowGroups;
	string rowGroupJson;

	uint i = 0;
	while (getNextJSONObj(rowScoutFileStream, rowGroupJson)) {
		JS::ParseContext context(rowGroupJson);

		RowGroup rowGroup;
		context.parseTo(rowGroup);

		rowGroup.index_in_file = i++;
		rowGroups.push_back(rowGroup);
	}

	rowScoutFileStream.close();

	return rowGroups;
}
