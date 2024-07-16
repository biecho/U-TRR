#pragma once

#include <vector>

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
std::vector<RowGroup> selectRowGroupsWithRetTimeConstraint(const std::vector<RowGroup> &rowGroups, const uint numRowGroups, const uint maxAllowedRetTimeDiff);
