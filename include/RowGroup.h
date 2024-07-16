#pragma once

#include <bitset>
#include <cstdlib>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace std;

typedef struct RowData {
	bitset<512> input_data_pattern;
	uint pattern_id;
	string label;
} RowData;

typedef struct Row {
	uint row_id{};
	std::vector<uint> bitflip_locs;

	Row() = default;
	Row(uint _row_id, vector<uint> _bitflip_locs)
		: row_id(_row_id)
		, bitflip_locs(std::move(_bitflip_locs))
	{
	}
} Row;

typedef struct RowGroup {
	std::vector<Row> rows;
	uint bank_id;
	uint ret_ms;
	uint index_in_file;
	uint data_pattern_type;
	uint rowdata_ind;

	RowGroup() = default;

	RowGroup(std::vector<Row> _weak_rows, uint _bank_id, uint _ret_ms,
		 uint _data_pattern_type, uint _rowdata_ind)
		: rows(std::move(_weak_rows))
		, bank_id(_bank_id)
		, ret_ms(_ret_ms)
		, data_pattern_type(_data_pattern_type)
		, rowdata_ind(_rowdata_ind)
	{
	}

	bool hasCommonRowWith(const RowGroup &other) const
	{
		for (const auto &current_row : rows) {
			for (const auto &other_row : other.rows) {
				if (current_row.row_id == other_row.row_id) {
					return true;
				}
			}
		}

		return false;
	}

	std::string toString()
	{
		std::ostringstream ss;
		ss << "(";

		if (!rows.empty()) {
			// Append all but the last row ID followed by ", "
			for (size_t i = 0; i < rows.size() - 1; ++i) {
				ss << rows[i].row_id << ", ";
			}
			// Append the last row ID without ", "
			ss << rows.back().row_id;
		}

		ss << ")";
		return ss.str();
	}

	std::string rows_as_str()
	{
		std::string ret = "(";

		for (auto &wr : rows) {
			ret = ret + to_string(wr.row_id) + ", ";
		}

		// remove ', ' at the end
		if (ret.size() > 1) {
			ret = ret.substr(0, ret.size() - 2);
		}

		ret = ret + ")";

		return ret;
	}
} RowGroup;

/**
 * @brief Selects a subset of row groups such that the difference in retention times between the
 * groups is within a specified limit.
 *
 * @param rowGroups A vector of RowGroup objects from which to select.
 * @param numRowGroups The number of row groups to select.
 * @param maxAllowedRetTimeDiff The maximum allowed difference in retention times between any two
 * selected row groups.
 * @return A vector of selected RowGroup objects meeting the retention time constraint.
 */
std::vector<RowGroup> selectRowGroupsWithRetTimeConstraint(std::vector<RowGroup> &rowGroups,
							   uint numRowGroups,
							   uint maxAllowedRetTimeDiff);
