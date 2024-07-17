#include "instruction.h"
#include "prog.h"
#include "platform.h"
#include "json_struct.h"
#include "softmc_utils.h"
#include "ProgressBar.hpp"

#include <string>
#include <iostream>
#include <list>
#include <cassert>
#include <bitset>
#include <chrono>
#include <stdexcept>
#include <iostream>
#include <random>

#include "RowGroup.h"
#include "Dram.h"
#include "MemoryAnalysis.h"
#include "Colors.h"
#include "BitUtils.h"
#include "TRRAnalyzer/config.h"

#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
using namespace boost::program_options;
using namespace boost::filesystem;

#include <array>
#include <algorithm>
#include <numeric>
#include <regex>

// #define PRINT_SOFTMC_PROGS

using namespace std;

// TRR Analyzer Parameters
const float TRR_RETTIME_MULT = 1.2f;
const uint TRR_CHECK_HAMMERS = 500000;
const uint TRR_DUMMY_ROW_DIST = 2; // the minimum row distance between dummy rows
const uint TRR_WEAK_DUMMY_DIST = 5000; // the minimum row distance between weak and dummy aggressor
				       // rows
const uint TRR_ALLOWED_RET_TIME_DIFF = 64;

vector<uint32_t> reserved_regs{ CASR, BASR, RASR };

typedef struct HammerableRowSet {
	std::vector<uint> victim_ids;
	std::vector<std::vector<uint> > vict_bitflip_locs;
	std::vector<std::vector<uint> > uni_bitflip_locs;
	std::vector<uint> aggr_ids;
	std::vector<uint> uni_ids;
	bitset<512> data_pattern;
	uint bank_id{};
	uint ret_ms{};
} HammerableRowSet;

struct RowInitializationData {
	vector<uint> rows;
	vector<bitset<512> > patterns;
};

/**
 * Initializes data rows in memory using specific data patterns.
 *
 * This function iterates through each given row, sets up the necessary registers and data patterns,
 * and performs the memory operations to initialize the rows. It ensures that register allocations
 * are properly managed and that each memory operation adheres to specified timings.
 */
void init_row_data(Program &prog, SoftMCRegAllocator &reg_alloc, const SMC_REG reg_bank_addr,
		   const SMC_REG reg_num_cols, RowInitializationData rowInitData)
{
	uint initial_free_regs = reg_alloc.num_free_regs();

	bitset<512> bitset_int_mask(0xFFFFFFFF);

	prog.add_inst(SMC_LI(8, CASR)); // Load 8 into CASR since each WRITE writes 8 columns
	SMC_REG reg_row_addr = reg_alloc.allocate_SMC_REG();
	SMC_REG reg_col_addr = reg_alloc.allocate_SMC_REG();

	auto rows_to_init = rowInitData.rows;
	auto data_patts = rowInitData.patterns;

	assert(rows_to_init.size() == data_patts.size());
	for (uint i = 0; i < rows_to_init.size(); i++) {
		uint target_row = rows_to_init[i];

		prog.add_inst(SMC_LI(target_row, reg_row_addr));

		if (i == 0 || data_patts[i - 1] != data_patts[i]) {
			// set up the input data in the wide register
			SMC_REG reg_wrdata = reg_alloc.allocate_SMC_REG();
			for (int pos = 0; pos < 16; pos++) {
				prog.add_inst(SMC_LI(
					(((data_patts[i] >> 32 * pos) & bitset_int_mask).to_ulong() &
					 0xFFFFFFFF),
					reg_wrdata));
				prog.add_inst(SMC_LDWD(reg_wrdata, pos));
			}
			reg_alloc.free_SMC_REG(reg_wrdata);
		}

		// activate the target row
		uint remaining = add_op_with_delay(prog, SMC_ACT(reg_bank_addr, 0, reg_row_addr, 0),
						   0, trcd_cycles - 5);

		// write data to the row and precharge
		add_op_with_delay(prog, SMC_LI(0, reg_col_addr), remaining, 0);

		string new_lbl = createSMCLabel("INIT_ROW");
		prog.add_label(new_lbl);
		add_op_with_delay(prog, SMC_WRITE(reg_bank_addr, 0, reg_col_addr, 1, 0, 0), 0, 0);
		prog.add_branch(prog.BR_TYPE::BL, reg_col_addr, reg_num_cols, new_lbl);

		// precharge the open bank
		add_op_with_delay(prog, SMC_PRE(reg_bank_addr, 0, 0), 0, trp_cycles);
	}

	reg_alloc.free_SMC_REG(reg_row_addr);
	reg_alloc.free_SMC_REG(reg_col_addr);

	assert(reg_alloc.num_free_regs() == initial_free_regs);
}

void hammerAggressors(Program &prog, SoftMCRegAllocator &reg_alloc, SMC_REG reg_bank_addr,
		      const vector<uint> &rows_to_hammer, const std::vector<uint> &num_hammers,
		      bool cascaded_hammer, uint hammer_duration);

void hammerAggressorsCascade(Program &prog, SoftMCRegAllocator &reg_alloc, SMC_REG reg_bank_addr,
			     const vector<uint> &rows_to_hammer, const vector<uint> &num_hammers,
			     uint hammer_duration);

void hammerAggressorsInterleaved(Program &prog, SoftMCRegAllocator &reg_alloc,
				 SMC_REG reg_bank_addr, const vector<uint> &rows_to_hammer,
				 const vector<uint> &num_hammers, uint hammer_duration);

RowInitializationData createRowInitializationData(const HammerableRowSet &hr,
						  const bool init_aggrs_first,
						  const bool ignore_aggrs,
						  const bool init_only_victims)
{
	vector<uint> rows_to_init;
	vector<bitset<512> > data_patts;

	uint total_rows = hr.victim_ids.size() + hr.aggr_ids.size() + hr.uni_ids.size();
	rows_to_init.reserve(total_rows);
	data_patts.reserve(total_rows);

	for (auto &vict : hr.victim_ids) {
		rows_to_init.push_back(vict);
		data_patts.push_back(hr.data_pattern);
	}

	for (auto &uni : hr.uni_ids) {
		rows_to_init.push_back(uni);
		data_patts.push_back(hr.data_pattern);
	}

	bitset<512> aggr_data_patt = hr.data_pattern;
	aggr_data_patt.flip();
	assert(aggr_data_patt != hr.data_pattern);

	if (!ignore_aggrs && !init_only_victims) {
		for (auto &aggr : hr.aggr_ids) {
			if (init_aggrs_first) {
				rows_to_init.insert(rows_to_init.begin(), aggr);
				data_patts.insert(data_patts.begin(), aggr_data_patt);
			} else {
				rows_to_init.push_back(aggr);
				data_patts.push_back(aggr_data_patt);
			}
		}
	}

	return { rows_to_init, data_patts };
}

void init_HRS_data(Program &prog, SoftMCRegAllocator &reg_alloc, const SMC_REG reg_bank_addr,
		   const SMC_REG reg_num_cols, const HammerableRowSet &hr,
		   const bool init_aggrs_first, const bool ignore_aggrs,
		   const bool init_only_victims)
{
	// initialize data of the two rows on the sides as well (if they are not out of bounds)
	// we do this because these rows can be aggressor rows in the TRRAnalyzer experiments, and
	// they may affect the retention of the first and last rows. Since we collect the first
	// retention failures using a big batch of rows (where these side rows are likely to be
	// initialized), we should do the same here

	// As a slight change to the note above, let's not initialize the aggressors on the sides
	// (unless the rh_type forces that)
	// to make it easier to find which ACTs are sampling in Hynix modules

	// init_data_row_range(prog, reg_alloc, reg_bank_addr, reg_num_cols, first_row_id,
	// last_row_id, hr.data_pattern);

	// UPDATE 09.12.2020 - Instead of initializing a range of rows, now we initilize the victims
	// first and then the aggressors one by one. This is to make aggressors the last row
	// activated prior to a refresh when no rows are hammering during the hammering phase. This
	// change is useful for analyzing the sampling method of Hynix modules
	Program &prog1 = prog;
	SoftMCRegAllocator &regAlloc = reg_alloc;
	auto rowInitData =
		createRowInitializationData(hr, init_aggrs_first, ignore_aggrs, init_only_victims);

	init_row_data(prog1, regAlloc, reg_bank_addr, reg_num_cols, rowInitData);
}

void hammerAggressors(Program &prog, SoftMCRegAllocator &reg_alloc, const SMC_REG reg_bank_addr,
		      const vector<uint> &rows_to_hammer, const std::vector<uint> &num_hammers,
		      const bool cascaded_hammer, const uint hammer_duration)
{
	if (!cascaded_hammer) {
		hammerAggressorsInterleaved(prog, reg_alloc, reg_bank_addr, rows_to_hammer,
					    num_hammers, hammer_duration);

	} else {
		hammerAggressorsCascade(prog, reg_alloc, reg_bank_addr, rows_to_hammer, num_hammers,
					hammer_duration);
	}
}

/**
 * Performs an interleaved Row Hammer attack on a specified set of rows within a memory bank.
 * This function cycles through the rows, hammering each one in rounds according to the minimum
 * number of hammers needed across all rows, ensuring each row is hammered uniformly over time.
 *
 * The function first checks if there are any rows to hammer. If not, it exits early.
 * It then repeatedly cycles through each row, applying a number of hammers based on the smallest
 * non-zero hammer count in the current round. This interleaving continues until all specified
 * hammer counts are exhausted. The function ensures each row is hammered the correct number
 * of times as efficiently as possible by adjusting the operations based on remaining hammer counts.
 */
void hammerAggressorsInterleaved(Program &prog, SoftMCRegAllocator &reg_alloc,
				 const SMC_REG reg_bank_addr, const vector<uint> &rows_to_hammer,
				 const vector<uint> &num_hammers, const uint hammer_duration)
{
	if (rows_to_hammer.empty())
		return; // nothing to hammer

	uint initial_free_regs = reg_alloc.num_free_regs();
	uint remaining_cycs = 0;

	SMC_REG reg_row_addr = reg_alloc.allocate_SMC_REG();
	SMC_REG reg_cur_hammers = reg_alloc.allocate_SMC_REG();
	SMC_REG reg_num_hammers = reg_alloc.allocate_SMC_REG();

	// it is complicated to efficiently hammer rows different number of times while
	// activating them one after another We implement the following algorithm:
	// 1. If there is a non-zero element in hammer_per_ref, hammer the rows
	// corresponding to those elements using the smallest non-zero hammer_per_ref value.
	// If all hammer_per_ref elements are zero, exit
	// 2. decrement all non-zero elements of hammer_per_ref vector by the smallest value
	// 3. go back to 1

	auto hammers_per_round = num_hammers;

	while (true) {
		auto min_non_zero = min_element(hammers_per_round.begin(), hammers_per_round.end(),
						[](const uint &a, const uint &b) {
							return ((a > 0) && (a < b)) || (b == 0);
						});

		if (min_non_zero == hammers_per_round.end() || *min_non_zero == 0) {
			break;
		}

		uint min_elem = *min_non_zero;

		// perform hammering
		prog.add_inst(SMC_LI(min_elem, reg_num_hammers));
		prog.add_inst(SMC_LI(0, reg_cur_hammers));
		string lbl_rh = createSMCLabel("ROWHAMMERING");
		prog.add_label(lbl_rh);
		for (int ind_row = 0; ind_row < rows_to_hammer.size(); ind_row++) {
			if (hammers_per_round[ind_row] == 0) // do not anymore hammer a row
							     // that has 0 remaining hammers
				continue;

			int row_id = rows_to_hammer[ind_row];
			prog.add_inst(SMC_LI(row_id, reg_row_addr));

			if (hammer_duration < 20)
				remaining_cycs = add_op_with_delay(
					prog, SMC_ACT(reg_bank_addr, 0, reg_row_addr, 0), 0,
					tras_cycles + hammer_duration - 1);
			else {
				remaining_cycs = add_op_with_delay(
					prog, SMC_ACT(reg_bank_addr, 0, reg_row_addr, 0), 0,
					hammer_duration % 4);
				remaining_cycs = add_op_with_delay(
					prog, SMC_SLEEP(floor(hammer_duration / 4.0f)),
					remaining_cycs, tras_cycles - 1);
			}

			remaining_cycs = add_op_with_delay(prog, SMC_PRE(reg_bank_addr, 0, 0), 0,
							   trp_cycles - 5);
		}

		prog.add_inst(SMC_ADDI(reg_cur_hammers, 1, reg_cur_hammers));
		prog.add_branch(Program::BL, reg_cur_hammers, reg_num_hammers, lbl_rh);

		// this subtracts min_elem from every non-zero element
		for_each(hammers_per_round.begin(), hammers_per_round.end(), [&](uint &a) {
			if (a > 0)
				a -= min_elem;
		});
	}

	reg_alloc.free_SMC_REG(reg_row_addr);
	reg_alloc.free_SMC_REG(reg_cur_hammers);
	reg_alloc.free_SMC_REG(reg_num_hammers);

	assert(reg_alloc.num_free_regs() == initial_free_regs);
}

/**
 * Simulates a Row Hammer attack on specified rows within a memory bank. This function performs
 * consecutive hammering operations on each row, based on the provided counts, without interleaving
 * between rows.
 */
void hammerAggressorsCascade(Program &prog, SoftMCRegAllocator &reg_alloc,
			     const SMC_REG reg_bank_addr, const vector<uint> &rows_to_hammer,
			     const vector<uint> &num_hammers, const uint hammer_duration)
{
	if (rows_to_hammer.empty())
		return; // nothing to hammer

	uint initial_free_regs = reg_alloc.num_free_regs();
	uint remaining_cycs = 0;

	SMC_REG reg_row_addr = reg_alloc.allocate_SMC_REG();
	SMC_REG reg_cur_hammers = reg_alloc.allocate_SMC_REG();
	SMC_REG reg_num_hammers = reg_alloc.allocate_SMC_REG();

	for (int ind_row = 0; ind_row < rows_to_hammer.size(); ind_row++) {
		int row_id = rows_to_hammer[ind_row];

		if (num_hammers[ind_row] == 0) // do not hammer rows with 0 hammer count
			continue;

		prog.add_inst(SMC_LI(row_id, reg_row_addr));
		prog.add_inst(SMC_LI(num_hammers[ind_row], reg_num_hammers));
		prog.add_inst(SMC_LI(0, reg_cur_hammers));

		string lbl_rh = createSMCLabel("ROWHAMMERING");
		prog.add_label(lbl_rh);

		if (hammer_duration < 20)
			remaining_cycs =
				add_op_with_delay(prog, SMC_ACT(reg_bank_addr, 0, reg_row_addr, 0),
						  0, tras_cycles + hammer_duration - 1);
		else {
			remaining_cycs = add_op_with_delay(
				prog, SMC_ACT(reg_bank_addr, 0, reg_row_addr, 0), 0, 0);
			remaining_cycs = add_op_with_delay(
				prog, SMC_SLEEP(ceil(hammer_duration / 4.0f)), 0, tras_cycles - 5);
		}

		remaining_cycs = add_op_with_delay(prog, SMC_PRE(reg_bank_addr, 0, 0), 0, 0);
		remaining_cycs = 0;
		prog.add_inst(SMC_ADDI(reg_cur_hammers, 1, reg_cur_hammers));
		prog.add_branch(Program::BL, reg_cur_hammers, reg_num_hammers, lbl_rh);
	}

	reg_alloc.free_SMC_REG(reg_row_addr);
	reg_alloc.free_SMC_REG(reg_cur_hammers);
	reg_alloc.free_SMC_REG(reg_num_hammers);

	assert(reg_alloc.num_free_regs() == initial_free_regs);
}

void init_HRS_data(SoftMCPlatform &platform, const std::vector<HammerableRowSet> &vec_hr,
		   const bool init_aggrs_first, const bool ignore_aggrs,
		   const bool init_only_victims, const uint num_pre_init_bank0_hammers,
		   const uint pre_init_nops, Program *prog = nullptr,
		   SoftMCRegAllocator *reg_alloc = nullptr)
{
	bool exec_prog_and_clean = false;
	if (prog == nullptr) {
		prog = new Program();
		reg_alloc = new SoftMCRegAllocator(NUM_SOFTMC_REGS, reserved_regs);
		exec_prog_and_clean = true;
	}

	if (exec_prog_and_clean)
		add_op_with_delay(*prog, SMC_PRE(0, 0, 1), 0, 0); // precharge all banks

	SMC_REG reg_bank_addr = reg_alloc->allocate_SMC_REG();
	SMC_REG reg_num_cols = reg_alloc->allocate_SMC_REG();

	if (num_pre_init_bank0_hammers > 0) {
		prog->add_inst(SMC_LI(0, reg_bank_addr));
		std::vector<uint> rows_to_hammer = std::vector<uint>{ 0 };
		std::vector<uint> bank0_hammers_per_ref =
			std::vector<uint>{ num_pre_init_bank0_hammers };
		hammerAggressors(*prog, *reg_alloc, reg_bank_addr, rows_to_hammer,
				 bank0_hammers_per_ref, true, 0);
	}

	if (pre_init_nops > 0) {
		if (pre_init_nops < 3) {
			for (uint i = 0; i < pre_init_nops; i++)
				prog->add_inst(__pack_mininsts(SMC_NOP(), SMC_NOP(), SMC_NOP(),
							       SMC_NOP()));
		} else {
			prog->add_inst(SMC_SLEEP(pre_init_nops));
		}
	}

	prog->add_inst(SMC_LI(NUM_COLS_PER_ROW * 8, reg_num_cols));

	for (const auto &hrs : vec_hr) {
		prog->add_inst(SMC_LI(hrs.bank_id, reg_bank_addr));
		init_HRS_data(*prog, *reg_alloc, reg_bank_addr, reg_num_cols, hrs, init_aggrs_first,
			      ignore_aggrs, init_only_victims);
	}

	reg_alloc->free_SMC_REG(reg_bank_addr);
	reg_alloc->free_SMC_REG(reg_num_cols);

	if (exec_prog_and_clean) {
		prog->add_inst(SMC_END());
		platform.execute(*prog);
		delete prog;
		delete reg_alloc;
	}
}

void read_row_data(Program &prog, SoftMCRegAllocator &reg_alloc, const SMC_REG reg_bank_addr,
		   const SMC_REG reg_num_cols, const vector<uint> &rows_to_read)
{
	uint initial_free_regs = reg_alloc.num_free_regs();

	prog.add_inst(SMC_LI(8, CASR)); // Load 8 into CASR since each READ reads 8 columns
	SMC_REG reg_row_addr = reg_alloc.allocate_SMC_REG();

	for (auto target_row : rows_to_read) {
		prog.add_inst(SMC_LI(target_row, reg_row_addr));

		// activate the victim row
		add_op_with_delay(prog, SMC_ACT(reg_bank_addr, 0, reg_row_addr, 0), 0,
				  trcd_cycles - 1);

		// read data from the row and precharge
		SMC_REG reg_col_addr = reg_alloc.allocate_SMC_REG();
		prog.add_inst(SMC_LI(0, reg_col_addr));

		string new_lbl = createSMCLabel("READ_ROW");
		prog.add_label(new_lbl);
		add_op_with_delay(prog, SMC_READ(reg_bank_addr, 0, reg_col_addr, 1, 0, 0), 0, 0);
		prog.add_branch(prog.BR_TYPE::BL, reg_col_addr, reg_num_cols, new_lbl);
		reg_alloc.free_SMC_REG(reg_col_addr);

		// precharge the open bank
		add_op_with_delay(prog, SMC_PRE(reg_bank_addr, 0, 0), 0, trp_cycles);
	}

	reg_alloc.free_SMC_REG(reg_row_addr);
	assert(reg_alloc.num_free_regs() == initial_free_regs);
}

HammerableRowSet toHammerableRowSet(const RowGroup &rowGroup, const std::string &rowLayout)
{
	HammerableRowSet hr;

	hr.bank_id = rowGroup.bank_id;
	hr.ret_ms = rowGroup.ret_ms;

	hr.victim_ids.reserve(rowGroup.rows.size());

	uint rowGroupIndex = 0;
	uint victToAggrDist = 1;
	for (const char rowType : rowLayout) {
		auto rowId = rowGroup.rows[rowGroupIndex].row_id;
		auto locs = rowGroup.rows[rowGroupIndex].bitflip_locs;

		switch (rowType) {
		case 'r':
		case 'R':
			hr.victim_ids.push_back(rowId);
			hr.vict_bitflip_locs.push_back(locs);
			rowGroupIndex++;
			victToAggrDist = 1;
			break;
		case 'a':
		case 'A': {
			auto id = to_physical_row_id(hr.victim_ids.back()) + victToAggrDist;
			hr.aggr_ids.push_back(to_logical_row_id(id));

			// advance rowGroupIndex if the corresponding row is profiled as a
			// retention weak row but we would like to use it as an aggressor row
			if (hr.aggr_ids.back() == rowId) {
				rowGroupIndex++;
			}

			victToAggrDist = 1;
			break;
		}
		case 'u':
		case 'U':
			hr.uni_ids.push_back(rowId);
			hr.uni_bitflip_locs.push_back(locs);
			rowGroupIndex++;
			victToAggrDist = 1;
			break;
		case '-':
			victToAggrDist++;
			break;

		default:
			std::cerr << RED_TXT
				  << "ERROR: Unexpected character in rowLayout. rowLayout: "
				  << rowLayout << ", unexpected char: " << rowType << NORMAL_TXT
				  << std::endl;
			break;
		}
	}

	hr.data_pattern = generateDataPattern(rowGroup.data_pattern_type);
	// for aggressor rows we use the bit inverse of victim's data pattern

	return hr;
}

// runs a small test that checks whether the weak rows in wrs can be hammered using aggressor rows
// determined based on the rh_type. If the aggressor rows are physically close to the weak rows,
// then we should observe RowHammer bitflips.
bool is_hammerable(SoftMCPlatform &platform, const RowGroup &wrs, const std::string row_layout,
		   const bool cascaded_hammer)
{
	Program p_testRH;
	uint target_bank = wrs.bank_id;

	/****************************/
	/*** Initialize DRAM data ***/
	/****************************/

	int remaining_cycs = 0;

	SoftMCRegAllocator reg_alloc(NUM_SOFTMC_REGS, reserved_regs);

	SMC_REG reg_bank_addr = reg_alloc.allocate_SMC_REG();
	p_testRH.add_inst(SMC_LI(target_bank, reg_bank_addr));

	add_op_with_delay(p_testRH, SMC_PRE(reg_bank_addr, 0, 1), 0, 0); // precharge all banks

	SMC_REG reg_num_cols = reg_alloc.allocate_SMC_REG();
	p_testRH.add_inst(SMC_LI(NUM_COLS_PER_ROW * 8, reg_num_cols));

	p_testRH.add_inst(SMC_LI(8, CASR)); // Load 8 into CASR since each READ reads 8 columns
	p_testRH.add_inst(SMC_LI(1, BASR)); // Load 1 into BASR
	p_testRH.add_inst(SMC_LI(1, RASR)); // Load 1 into RASR

	HammerableRowSet hr = toHammerableRowSet(wrs, row_layout);

	// no need to check if the WRS is hammerable if there are no aggressor rows
	// A WRS won't have aggressor rows when using the 'R' row layout and BETWEEN_VICTIMS RHType
	if ((hr.aggr_ids.size() + hr.uni_ids.size()) == 0)
		return true;

	// write to the victim and aggressor row(s)
	// first all victims, and then the aggressors are initialized
	Program &prog = p_testRH;
	SoftMCRegAllocator &regAlloc = reg_alloc;
	auto rowInitData = createRowInitializationData(hr, false, false, false);

	init_row_data(prog, regAlloc, reg_bank_addr, reg_num_cols, rowInitData);

	/*************************/
	/*** Perform Hammering ***/
	/*************************/

	vector<uint> hammers(hr.aggr_ids.size(), TRR_CHECK_HAMMERS);
	hammerAggressors(p_testRH, reg_alloc, reg_bank_addr, hr.aggr_ids, hammers, cascaded_hammer,
			 0);

	/********************************/
	/*** issue DRAM READ commands ***/
	/********************************/
	read_row_data(p_testRH, reg_alloc, reg_bank_addr, reg_num_cols, hr.victim_ids);
	p_testRH.add_inst(SMC_END());

	platform.execute(p_testRH);

	/*********************************************/
	/*** read PCIe data and check for bitflips ***/
	/*********************************************/
	char buf[ROW_SIZE_BYTES * hr.victim_ids.size()];
	platform.receiveData(buf, ROW_SIZE_BYTES * hr.victim_ids.size());
	vector<uint> bitflips;

	// we expect all victim rows to be hammerable
	bool all_victims_have_bitflips = true;
	for (uint i = 0; i < hr.victim_ids.size(); i++) {
		bitflips = detectBitflips(buf + ROW_SIZE_BYTES, ROW_SIZE_BYTES, hr.data_pattern);
		if (bitflips.empty()) {
			all_victims_have_bitflips = false;
			std::cout << RED_TXT << "No RH bitflips found in row " << hr.victim_ids[i]
				  << NORMAL_TXT << std::endl;
		}
	}

	return all_victims_have_bitflips;
}

void pick_dummy_aggressors(vector<uint> &dummy_aggrs, const uint dummy_aggrs_bank,
			   const uint num_dummies, const vector<RowGroup> &weak_row_sets,
			   const uint dummy_ids_offset)
{
	uint cur_dummy = TRR_DUMMY_ROW_DIST % NUM_ROWS;
	if (weak_row_sets.size() != 0)
		cur_dummy =
			(weak_row_sets[0].rows[0].row_id + TRR_WEAK_DUMMY_DIST + dummy_ids_offset) %
			NUM_ROWS;

	const uint MAX_TRIES = 1000000;
	uint cur_try = 0;

	while (dummy_aggrs.size() != num_dummies) {
		if (cur_try == MAX_TRIES) {
			std::cerr << RED_TXT << "ERROR: Failed to pick dummy aggressor rows after "
				  << MAX_TRIES << " tries" << NORMAL_TXT << std::endl;
			std::cerr << "Consider reducing the number of weak or dummy rows"
				  << std::endl;
			exit(-1);
		}

		// check if there is a victim close to cur_dummy
		if (dummy_aggrs_bank == weak_row_sets[0].bank_id) {
			for (auto &wrs : weak_row_sets) {
				for (auto &wr : wrs.rows) {
					if (std::abs((int)cur_dummy - (int)wr.row_id) <
					    TRR_WEAK_DUMMY_DIST) {
						cur_dummy = (cur_dummy + TRR_WEAK_DUMMY_DIST) %
							    NUM_ROWS;
						cur_try++;
						continue;
					}
				}
			}
		}

		dummy_aggrs.push_back(cur_dummy);
		cur_dummy = (cur_dummy + TRR_DUMMY_ROW_DIST) % NUM_ROWS;
	}
}

void perform_refresh(Program &prog, SoftMCRegAllocator &reg_alloc, const uint num_refs_per_round,
		     const uint pre_ref_delay)
{
	SMC_REG reg_num_refs_per_cycle = reg_alloc.allocate_SMC_REG();
	SMC_REG reg_it_refs_per_cycle = reg_alloc.allocate_SMC_REG();

	prog.add_inst(SMC_LI(num_refs_per_round, reg_num_refs_per_cycle));
	prog.add_inst(SMC_LI(0, reg_it_refs_per_cycle));

	if (pre_ref_delay >= 8) {
		prog.add_inst(SMC_SLEEP(std::ceil(pre_ref_delay / 4.0f)));
	}

	std::string lbl_issue_per_cycle_refs = createSMCLabel("PER_CYCLE_REFS");
	prog.add_label(lbl_issue_per_cycle_refs);
	add_op_with_delay(prog, SMC_REF(), 0, 0);
	add_op_with_delay(prog, SMC_SLEEP(ceil((trfc_cycles - 1 - 24 - 4) / 4.0f)), 0, 0);
	add_op_with_delay(prog, SMC_ADDI(reg_it_refs_per_cycle, 1, reg_it_refs_per_cycle), 0, 0);
	prog.add_branch(prog.BR_TYPE::BL, reg_it_refs_per_cycle, reg_num_refs_per_cycle,
			lbl_issue_per_cycle_refs);

	reg_alloc.free_SMC_REG(reg_num_refs_per_cycle);
	reg_alloc.free_SMC_REG(reg_it_refs_per_cycle);
}

void issue_REFs(SoftMCPlatform &platform, const uint num_refs, Program *prog = nullptr,
		SoftMCRegAllocator *reg_alloc = nullptr)
{
	bool exec_prog_and_clean = false;
	if (prog == nullptr) {
		prog = new Program();
		reg_alloc = new SoftMCRegAllocator(NUM_SOFTMC_REGS, reserved_regs);
		exec_prog_and_clean = true;
	}

	SMC_REG reg_num_refs = reg_alloc->allocate_SMC_REG();
	SMC_REG reg_issued_refs = reg_alloc->allocate_SMC_REG();

	prog->add_inst(SMC_LI(num_refs, reg_num_refs));
	prog->add_inst(SMC_LI(0, reg_issued_refs));

	int remaining_cycs = 0;
	if (exec_prog_and_clean)
		remaining_cycs =
			add_op_with_delay(*prog, SMC_PRE(0, 0, 1), 0, trp_cycles); // precharge
										   // all
										   // banks

	if (remaining_cycs > 0) {
		remaining_cycs = add_op_with_delay(*prog, SMC_NOP(), remaining_cycs, 0);
	}

	std::string lbl_issue_refs = createSMCLabel("ISSUE_REFS");
	prog->add_label(lbl_issue_refs);

	add_op_with_delay(*prog, SMC_REF(), 0, 0);

	// the SLEEP function waits for 1 SoftMC frontend logic cycle (4 * FPGA_PERIOD). Therefore,
	// we divide by 4
	add_op_with_delay(*prog, SMC_SLEEP(ceil((trefi_cycles - 4 - 24 - 3) / 4.0f)), 0, 0);

	prog->add_inst(SMC_ADDI(reg_issued_refs, 1, reg_issued_refs));
	prog->add_branch(prog->BR_TYPE::BL, reg_issued_refs, reg_num_refs, lbl_issue_refs);

	if (exec_prog_and_clean) {
		// perform a dummy read to signal the end of a program

		SMC_REG reg_bank_id = reg_alloc->allocate_SMC_REG();
		SMC_REG reg_row_id = reg_alloc->allocate_SMC_REG();
		SMC_REG reg_col_id = reg_alloc->allocate_SMC_REG();
		add_op_with_delay(*prog, SMC_LI(0, reg_bank_id), 0, 0);
		add_op_with_delay(*prog, SMC_LI(0, reg_row_id), 0, 0);
		add_op_with_delay(*prog, SMC_LI(0, reg_col_id), 0, 0);

		add_op_with_delay(*prog, SMC_ACT(reg_bank_id, 0, reg_row_id, 0), 0, trcd_cycles);
		add_op_with_delay(*prog, SMC_READ(reg_bank_id, 0, reg_col_id, 0, 0, 0), 0,
				  tras_cycles - trcd_cycles);
		add_op_with_delay(*prog, SMC_PRE(reg_bank_id, 0, 0), 0, trp_cycles);

		prog->add_inst(SMC_END());
		platform.execute(*prog);

		reg_alloc->free_SMC_REG(reg_bank_id);
		reg_alloc->free_SMC_REG(reg_row_id);
		reg_alloc->free_SMC_REG(reg_col_id);

		char cl[64];
		platform.receiveData(cl, 64);
	}

	reg_alloc->free_SMC_REG(reg_num_refs);
	reg_alloc->free_SMC_REG(reg_issued_refs);

	if (exec_prog_and_clean) {
		delete prog;
		delete reg_alloc;
	}
}

// performing REF at nominal rate, i.e., a REF cmd is issued once every 7.8us
// dummy rows are hammered between the REF cmds
void hammer_dummies(SoftMCPlatform &platform, const uint bank_id, const vector<uint> &dummy_aggrs,
		    const uint num_refs, Program *prog = nullptr,
		    SoftMCRegAllocator *reg_alloc = nullptr)
{
	bool exec_prog_and_clean = false;
	if (prog == nullptr) {
		prog = new Program();
		reg_alloc = new SoftMCRegAllocator(NUM_SOFTMC_REGS, reserved_regs);
		exec_prog_and_clean = true;
	}

	SMC_REG reg_num_refs = reg_alloc->allocate_SMC_REG();
	SMC_REG reg_issued_refs = reg_alloc->allocate_SMC_REG();

	SMC_REG reg_row_id = reg_alloc->allocate_SMC_REG();
	SMC_REG reg_bank_id = reg_alloc->allocate_SMC_REG();
	prog->add_inst(SMC_LI(bank_id, reg_bank_id));

	prog->add_inst(SMC_LI(num_refs, reg_num_refs));
	prog->add_inst(SMC_LI(0, reg_issued_refs));

	int remaining_cycs = 0;
	if (exec_prog_and_clean)
		remaining_cycs = add_op_with_delay(*prog, SMC_PRE(0, 0, 1), 0, 0); // precharge all
										   // banks

	uint num_dummies = dummy_aggrs.size();
	uint cycs_hammer_dummies_once = (tras_cycles + trp_cycles) * num_dummies + 28;
	uint hammers_per_round = floor((trefi_cycles - trfc_cycles) / cycs_hammer_dummies_once);

	if (remaining_cycs > 0) {
		remaining_cycs = add_op_with_delay(*prog, SMC_NOP(), remaining_cycs, 0);
	}

	std::string lbl_issue_refs = createSMCLabel("ISSUE_REFS");
	prog->add_label(lbl_issue_refs);

	add_op_with_delay(*prog, SMC_REF(), 0, trfc_cycles - 1);

	// hammer the dummy rows
	SMC_REG reg_hammer_it = reg_alloc->allocate_SMC_REG();
	SMC_REG reg_hammers_per_ref = reg_alloc->allocate_SMC_REG();
	prog->add_inst(SMC_LI(0, reg_hammer_it));
	prog->add_inst(SMC_LI(hammers_per_round, reg_hammers_per_ref));

	std::string lbl_hammer = createSMCLabel("AFTER_INIT_DUMMY_HAMMERING");
	prog->add_label(lbl_hammer);
	remaining_cycs = 0;
	for (uint dummy_row_id : dummy_aggrs) {
		prog->add_inst(SMC_LI(dummy_row_id, reg_row_id));

		remaining_cycs = add_op_with_delay(*prog, SMC_ACT(reg_bank_id, 0, reg_row_id, 0),
						   remaining_cycs, tras_cycles - 1);
		remaining_cycs = add_op_with_delay(*prog, SMC_PRE(reg_bank_id, 0, 0),
						   remaining_cycs, trp_cycles - 1);
	}

	add_op_with_delay(*prog, SMC_ADDI(reg_hammer_it, 1, reg_hammer_it), 0, 0);
	prog->add_branch(prog->BR_TYPE::BL, reg_hammer_it, reg_hammers_per_ref, lbl_hammer);

	prog->add_inst(SMC_ADDI(reg_issued_refs, 1, reg_issued_refs));
	prog->add_branch(prog->BR_TYPE::BL, reg_issued_refs, reg_num_refs, lbl_issue_refs);

	if (exec_prog_and_clean) {
		// perform a dummy read to signal the end of a program

		SMC_REG reg_col_id = reg_alloc->allocate_SMC_REG();
		add_op_with_delay(*prog, SMC_LI(0, reg_bank_id), 0, 0);
		add_op_with_delay(*prog, SMC_LI(0, reg_row_id), 0, 0);
		add_op_with_delay(*prog, SMC_LI(0, reg_col_id), 0, 0);

		add_op_with_delay(*prog, SMC_ACT(reg_bank_id, 0, reg_row_id, 0), 0, trcd_cycles);
		add_op_with_delay(*prog, SMC_READ(reg_bank_id, 0, reg_col_id, 0, 0, 0), 0,
				  tras_cycles - trcd_cycles);
		add_op_with_delay(*prog, SMC_PRE(reg_bank_id, 0, 0), 0, trp_cycles);

		prog->add_inst(SMC_END());
		platform.execute(*prog);

		reg_alloc->free_SMC_REG(reg_col_id);

		char cl[64];
		platform.receiveData(cl, 64);
	}

	reg_alloc->free_SMC_REG(reg_num_refs);
	reg_alloc->free_SMC_REG(reg_issued_refs);
	reg_alloc->free_SMC_REG(reg_bank_id);
	reg_alloc->free_SMC_REG(reg_row_id);
	reg_alloc->free_SMC_REG(reg_hammer_it);
	reg_alloc->free_SMC_REG(reg_hammers_per_ref);

	if (exec_prog_and_clean) {
		delete prog;
		delete reg_alloc;
	}
}

void hammer_hrs(SoftMCPlatform &platform, const vector<HammerableRowSet> &hammerable_rows,
		const std::vector<uint> &hammers_per_round, const bool cascaded_hammer,
		const uint num_rounds, const bool skip_hammering_aggr,
		const bool ignore_dummy_hammers, const uint hammer_duration,
		const uint num_refs_per_round, const uint pre_ref_delay,
		const vector<uint> &dummy_aggrs, const uint dummy_aggrs_bank,
		const bool hammer_dummies_first, const bool hammer_dummies_independently,
		const uint num_bank0_hammers = 0, Program *prog = nullptr,
		SoftMCRegAllocator *reg_alloc = nullptr)
{
	bool exec_prog_and_clean = false;
	if (prog == nullptr) {
		prog = new Program();
		reg_alloc = new SoftMCRegAllocator(NUM_SOFTMC_REGS, reserved_regs);
		exec_prog_and_clean = true;
	}

	SMC_REG reg_bank_addr = reg_alloc->allocate_SMC_REG();
	prog->add_inst(SMC_LI(hammerable_rows[0].bank_id, reg_bank_addr));

	if (exec_prog_and_clean)
		add_op_with_delay(*prog, SMC_PRE(reg_bank_addr, 0, 1), 0, 0); // precharge all banks

	SMC_REG reg_cur_its = reg_alloc->allocate_SMC_REG();
	SMC_REG reg_refresh_cycles = reg_alloc->allocate_SMC_REG();
	prog->add_inst(SMC_LI(0, reg_cur_its));
	prog->add_inst(SMC_LI(num_rounds, reg_refresh_cycles));

	string lbl_hammer_loop = createSMCLabel("HAMMER_MAIN_LOOP");
	prog->add_label(lbl_hammer_loop);

	// hammering all aggressor and dummy rows
	std::vector<uint> all_rows_to_hammer;
	std::vector<uint> weak_rows_to_hammer;

	if (hammer_dummies_first && !hammer_dummies_independently && !ignore_dummy_hammers)
		all_rows_to_hammer.insert(all_rows_to_hammer.end(), dummy_aggrs.begin(),
					  dummy_aggrs.end());

	for (auto &hrs : hammerable_rows) {
		if (!skip_hammering_aggr) {
			all_rows_to_hammer.insert(all_rows_to_hammer.end(), hrs.aggr_ids.begin(),
						  hrs.aggr_ids.end());
			all_rows_to_hammer.insert(all_rows_to_hammer.end(), hrs.uni_ids.begin(),
						  hrs.uni_ids.end());
		}
		weak_rows_to_hammer.insert(weak_rows_to_hammer.end(), hrs.aggr_ids.begin(),
					   hrs.aggr_ids.end());
		weak_rows_to_hammer.insert(weak_rows_to_hammer.end(), hrs.uni_ids.begin(),
					   hrs.uni_ids.end());
	}

	if (!hammer_dummies_first & !hammer_dummies_independently & !ignore_dummy_hammers)
		all_rows_to_hammer.insert(all_rows_to_hammer.end(), dummy_aggrs.begin(),
					  dummy_aggrs.end());

	uint total_hammers_per_ref = 0;

	std::vector<uint> t_dummy_hammers_per_ref;
	std::vector<uint> t_aggr_hammers_per_ref;
	if (hammer_dummies_first) {
		t_dummy_hammers_per_ref = std::vector<uint>(
			hammers_per_round.begin(), hammers_per_round.begin() + dummy_aggrs.size());
		t_aggr_hammers_per_ref = std::vector<uint>(
			hammers_per_round.begin() + dummy_aggrs.size(), hammers_per_round.end());
	} else {
		t_dummy_hammers_per_ref =
			std::vector<uint>(hammers_per_round.begin() + weak_rows_to_hammer.size(),
					  hammers_per_round.end());
		t_aggr_hammers_per_ref =
			std::vector<uint>(hammers_per_round.begin(),
					  hammers_per_round.begin() + weak_rows_to_hammer.size());
	}

	std::vector<uint> new_hammers_per_ref;

	if (hammer_dummies_first && !ignore_dummy_hammers && !hammer_dummies_independently)
		new_hammers_per_ref.insert(new_hammers_per_ref.end(),
					   t_dummy_hammers_per_ref.begin(),
					   t_dummy_hammers_per_ref.end());

	if (!skip_hammering_aggr)
		new_hammers_per_ref.insert(new_hammers_per_ref.end(),
					   t_aggr_hammers_per_ref.begin(),
					   t_aggr_hammers_per_ref.end());

	if (!hammer_dummies_first && !ignore_dummy_hammers && !hammer_dummies_independently)
		new_hammers_per_ref.insert(new_hammers_per_ref.end(),
					   t_dummy_hammers_per_ref.begin(),
					   t_dummy_hammers_per_ref.end());

	if (!hammer_dummies_independently)
		assert(new_hammers_per_ref.size() == all_rows_to_hammer.size());

	for (unsigned int hammer_count : new_hammers_per_ref) {
		total_hammers_per_ref += hammer_count;
	}

	if (total_hammers_per_ref == 0 && num_refs_per_round == 0)
		return; // no need to launch SoftMC program since there is nothing to do (i.e., to
			// rows to hammer and no refs to do)

	if (hammer_dummies_independently && hammer_dummies_first && !ignore_dummy_hammers) {
		prog->add_inst(SMC_LI(dummy_aggrs_bank, reg_bank_addr));

		std::vector<uint32_t> dummy_hammers_per_round;

		if (hammer_dummies_first)
			dummy_hammers_per_round = std::vector<uint32_t>(hammers_per_round.begin(),
									hammers_per_round.begin() +
										dummy_aggrs.size());
		else
			dummy_hammers_per_round = std::vector<uint32_t>(
				hammers_per_round.begin() + weak_rows_to_hammer.size(),
				hammers_per_round.end());

		hammerAggressors(*prog, *reg_alloc, reg_bank_addr, dummy_aggrs,
				 dummy_hammers_per_round, cascaded_hammer, 0);
	}

	if (total_hammers_per_ref > 0) {
		if (dummy_aggrs_bank != hammerable_rows[0].bank_id) {
			assert(cascaded_hammer &&
			       "ERROR: Dummy aggressors can be in a different bank only when using "
			       "cascaded hammering. This feature is not yet implemented for "
			       "sequential hammering.");

			if (hammer_dummies_first && !hammer_dummies_independently &&
			    !ignore_dummy_hammers) {
				prog->add_inst(SMC_LI(dummy_aggrs_bank, reg_bank_addr));
				auto dummy_hammers_per_round = std::vector<uint32_t>(
					hammers_per_round.begin(),
					hammers_per_round.begin() + dummy_aggrs.size());
				hammerAggressors(*prog, *reg_alloc, reg_bank_addr, dummy_aggrs,
						 dummy_hammers_per_round, cascaded_hammer, 0);
			}

			if (!skip_hammering_aggr) {
				prog->add_inst(SMC_LI(hammerable_rows[0].bank_id, reg_bank_addr));

				auto weak_rows_hammers_per_ref = std::vector<uint32_t>(
					(hammer_dummies_first & !hammer_dummies_independently) ?
						hammers_per_round.begin() + dummy_aggrs.size() :
						hammers_per_round.begin(),
					(hammer_dummies_first & !hammer_dummies_independently) ?
						hammers_per_round.end() :
						hammers_per_round.begin() +
							weak_rows_to_hammer.size());
				hammerAggressors(*prog, *reg_alloc, reg_bank_addr,
						 weak_rows_to_hammer, weak_rows_hammers_per_ref,
						 cascaded_hammer, hammer_duration);
			}

			if (!hammer_dummies_first && !hammer_dummies_independently &&
			    !ignore_dummy_hammers) {
				prog->add_inst(SMC_LI(dummy_aggrs_bank, reg_bank_addr));
				auto dummy_hammers_per_round = std::vector<uint32_t>(
					hammers_per_round.begin() + weak_rows_to_hammer.size(),
					hammers_per_round.end());
				hammerAggressors(*prog, *reg_alloc, reg_bank_addr, dummy_aggrs,
						 dummy_hammers_per_round, cascaded_hammer, 0);
			}

		} else {
			hammerAggressors(*prog, *reg_alloc, reg_bank_addr, all_rows_to_hammer,
					 new_hammers_per_ref, cascaded_hammer, hammer_duration);
		}
	}

	if (hammer_dummies_independently && !hammer_dummies_first && !ignore_dummy_hammers) {
		prog->add_inst(SMC_LI(dummy_aggrs_bank, reg_bank_addr));

		std::vector<uint32_t> dummy_hammers_per_round;

		if (hammer_dummies_first)
			dummy_hammers_per_round = std::vector<uint32_t>(hammers_per_round.begin(),
									hammers_per_round.begin() +
										dummy_aggrs.size());
		else
			dummy_hammers_per_round = std::vector<uint32_t>(
				hammers_per_round.begin() + weak_rows_to_hammer.size(),
				hammers_per_round.end());

		hammerAggressors(*prog, *reg_alloc, reg_bank_addr, dummy_aggrs,
				 dummy_hammers_per_round, cascaded_hammer, 0);
	}

	if (num_bank0_hammers > 0) {
		prog->add_inst(SMC_LI(0, reg_bank_addr));
		all_rows_to_hammer = std::vector<uint>{ 0 };
		std::vector<uint> bank0_hammers_per_ref = std::vector<uint>{ num_bank0_hammers };
		hammerAggressors(*prog, *reg_alloc, reg_bank_addr, all_rows_to_hammer,
				 bank0_hammers_per_ref, cascaded_hammer, hammer_duration);
	}

	// 4) issue a REF
	if (num_rounds > 0 && num_refs_per_round > 0) {
		perform_refresh(*prog, *reg_alloc, num_refs_per_round, pre_ref_delay);

		prog->add_inst(SMC_ADDI(reg_cur_its, 1, reg_cur_its));
		prog->add_branch(Program::BR_TYPE::BL, reg_cur_its, reg_refresh_cycles,
				 lbl_hammer_loop); // 5) repeat hammering + REF num_rounds times
	}

	if (exec_prog_and_clean) {
		prog->add_inst(SMC_END());
		platform.execute(*prog);
	}

	reg_alloc->free_SMC_REG(reg_cur_its);
	reg_alloc->free_SMC_REG(reg_refresh_cycles);
	reg_alloc->free_SMC_REG(reg_bank_addr);

	if (exec_prog_and_clean) {
		delete prog;
		delete reg_alloc;
	}
}

void waitMS_softmc(const uint ret_time_ms, Program *prog)
{
	// convert milliseconds to SoftMC cycles

	ulong cycs = std::ceil((ret_time_ms * 1000000) / FPGA_PERIOD);

	// cycs is the number of DDR cycles now, convert it to FPGA cycles by dividing it by 4
	cycs = std::ceil(cycs / 4.0f);

	prog->add_inst(SMC_SLEEP(cycs));
}

vector<vector<uint> >
analyzeTRR(SoftMCPlatform &platform, const vector<HammerableRowSet> &hammerable_rows,
	   const vector<uint> &dummy_aggrs, const uint dummy_aggrs_bank,
	   const uint dummy_hammers_per_round, const bool hammer_dummies_first,
	   const bool hammer_dummies_independently, const bool cascaded_hammer,
	   const std::vector<uint> &hammers_per_round, const float hammer_cycle_time,
	   const uint hammer_duration, const uint num_rounds, const bool skip_hammering_aggr,
	   const uint refs_after_init, const vector<uint> &after_init_dummies,
	   const bool init_aggrs_first, const bool ignore_aggrs, const bool init_only_victims,
	   const bool ignore_dummy_hammers, const bool first_it_aggr_init_and_hammer,
	   const bool refs_after_init_no_dummy_hammer, const uint num_refs_per_round,
	   const uint pre_ref_delay, const std::vector<uint> &hammers_before_wait,
	   const float init_to_hammerbw_delay, const uint num_bank0_hammers,
	   const uint num_pre_init_bank0_hammers, const uint pre_init_nops,
	   const bool use_single_softmc_prog, const uint num_iterations, const bool verbose)
{
	Program single_prog;
	SoftMCRegAllocator single_prog_reg_alloc =
		SoftMCRegAllocator(NUM_SOFTMC_REGS, reserved_regs);

	// create the iteration loop
	SMC_REG reg_iter_counter = single_prog_reg_alloc.allocate_SMC_REG();
	SMC_REG reg_num_iters = single_prog_reg_alloc.allocate_SMC_REG();

	add_op_with_delay(single_prog, SMC_PRE(reg_iter_counter, 0, 1), 0, 0); // precharge all
									       // banks
	single_prog.add_inst(SMC_LI(0, reg_iter_counter));
	single_prog.add_inst(SMC_LI(num_iterations, reg_num_iters));

	std::string lbl_iter_loop = createSMCLabel("MAIN_ITERATION_LOOP");
	single_prog.add_label(lbl_iter_loop);

	// // 1) initialize the data of the entire row range from the smallest row id to the largest
	// row id in each HammerableRowSet
	std::string lbl_init_end = createSMCLabel("INIT_ROWS_END");

	auto t_start_init_data = chrono::high_resolution_clock::now();
	if (!skip_hammering_aggr) {
		if (!use_single_softmc_prog)
			init_HRS_data(platform, hammerable_rows, init_aggrs_first, ignore_aggrs,
				      init_only_victims, num_pre_init_bank0_hammers, pre_init_nops);
		else {
			std::string lbl_init_all = createSMCLabel("INIT_ALL_ROWS");

			if (first_it_aggr_init_and_hammer) {
				SMC_REG reg_zero = single_prog_reg_alloc.allocate_SMC_REG();
				single_prog.add_inst(SMC_LI(0, reg_zero));
				single_prog.add_branch(Program::BR_TYPE::BEQ, reg_iter_counter,
						       reg_zero, lbl_init_all);
				single_prog_reg_alloc.free_SMC_REG(reg_zero);

				init_HRS_data(platform, hammerable_rows, init_aggrs_first,
					      ignore_aggrs, true /*init_only_victims*/,
					      num_pre_init_bank0_hammers, pre_init_nops,
					      &single_prog, &single_prog_reg_alloc);

				single_prog.add_branch(Program::BR_TYPE::JUMP, reg_iter_counter,
						       reg_zero, lbl_init_end);
			}

			single_prog.add_label(lbl_init_all);
			init_HRS_data(platform, hammerable_rows, init_aggrs_first, ignore_aggrs,
				      init_only_victims, num_pre_init_bank0_hammers, pre_init_nops,
				      &single_prog, &single_prog_reg_alloc);
		}
	}

	if (use_single_softmc_prog) {
		single_prog.add_label(lbl_init_end);
	}

	auto t_end_issue_prog = chrono::high_resolution_clock::now();
	chrono::duration<double, milli> prog_issue_duration(t_end_issue_prog - t_start_init_data);

	/*** OPTIONAL - issue REF commands after initializing data ***/
	// this is an attempt to reset any REF related state
	if (refs_after_init > 0) {
		// auto t_start_issue_refs = chrono::high_resolution_clock::now();
		if (after_init_dummies.empty()) {
			if (!use_single_softmc_prog)
				issue_REFs(platform, refs_after_init);
			else
				issue_REFs(platform, refs_after_init, &single_prog,
					   &single_prog_reg_alloc);
		} else {
			if (!use_single_softmc_prog)
				hammer_dummies(platform, dummy_aggrs_bank, after_init_dummies,
					       refs_after_init);
			else
				hammer_dummies(platform, dummy_aggrs_bank, after_init_dummies,
					       refs_after_init, &single_prog,
					       &single_prog_reg_alloc);
		}

		if (refs_after_init_no_dummy_hammer) {
			if (!use_single_softmc_prog)
				issue_REFs(platform, refs_after_init);
			else
				issue_REFs(platform, refs_after_init, &single_prog,
					   &single_prog_reg_alloc);
		}

		t_end_issue_prog = chrono::high_resolution_clock::now();
	}

	// 2) wait until half of the target retention time, i.e., hammering_start_time =
	// (ret_ms*H_MODIFIER - (num_rounds*refresh_cycle_time))/2
	// SMC_WAIT() allows sleeping up to only ~6 seconds need to use the system timer like in the
	// RetentionProfiler to sleep longer calculating the time for activating all aggressor rows
	// once

	uint num_all_aggrs = 0;
	for (auto &hr : hammerable_rows) {
		num_all_aggrs += hr.aggr_ids.size();
		num_all_aggrs += hr.uni_ids.size();
	}

	uint act_pre_cycles =
		std::ceil(std::max(DEFAULT_TRAS + DEFAULT_TRP + hammer_duration * FPGA_PERIOD,
				   hammer_cycle_time) /
			  FPGA_PERIOD);
	uint act_all_rows_cycles =
		act_pre_cycles * (num_all_aggrs + dummy_aggrs.size()) + 24 /*for the branch inst*/;

	// calculating overall time that will be spend on hammering and refreshing
	// NOTE: this is a bit of overestimation as we assume each aggressor and dummy will be
	// activated max. amount of times
	uint max_hammer_acts =
		*(std::max_element(hammers_per_round.begin(), hammers_per_round.end()));
	max_hammer_acts = std::max(max_hammer_acts, dummy_hammers_per_round);
	uint total_hammer_cycles = max_hammer_acts * act_all_rows_cycles +
				   trfc_cycles * num_refs_per_round + pre_ref_delay;

	float total_hammer_ms = std::ceil(total_hammer_cycles * FPGA_PERIOD) / 1000000.0f;

	uint wait_interval_ms =
		(hammerable_rows[0].ret_ms * TRR_RETTIME_MULT - (num_rounds * total_hammer_ms)) / 2;

	const uint c_wait_interval_ms = wait_interval_ms;

	if (verbose) {
		std::cout << YELLOW_TXT
			  << "Weak row retention time (ms): " << hammerable_rows[0].ret_ms
			  << NORMAL_TXT << std::endl;

		std::cout << YELLOW_TXT
			  << "Time to complete hammering phase (ms): " << total_hammer_ms
			  << NORMAL_TXT << std::endl;
	}

	// hammers the rows here as well if hammers_before_wait contains non zero element
	if (init_to_hammerbw_delay > 0.0f) {
		uint wait_ms = wait_interval_ms * init_to_hammerbw_delay;
		wait_interval_ms -= wait_ms;

		if (!use_single_softmc_prog)
			waitMS(wait_ms);
		else
			waitMS_softmc(wait_ms, &single_prog);
	}

	if (!hammers_before_wait.empty()) {
		if (!use_single_softmc_prog)
			hammer_hrs(platform, hammerable_rows, hammers_before_wait, cascaded_hammer,
				   1, skip_hammering_aggr | ignore_aggrs, ignore_dummy_hammers,
				   hammer_duration, 0, pre_ref_delay, dummy_aggrs, dummy_aggrs_bank,
				   hammer_dummies_first, hammer_dummies_independently);
		else
			hammer_hrs(platform, hammerable_rows, hammers_before_wait, cascaded_hammer,
				   1, skip_hammering_aggr | ignore_aggrs, ignore_dummy_hammers,
				   hammer_duration, 0, pre_ref_delay, dummy_aggrs, dummy_aggrs_bank,
				   hammer_dummies_first, hammer_dummies_independently, 0,
				   &single_prog, &single_prog_reg_alloc);
	}

	if (!skip_hammering_aggr) {
		if (!use_single_softmc_prog)
			waitMS(wait_interval_ms /* - prog_issue_duration.count()*/);
		else
			waitMS_softmc(wait_interval_ms, &single_prog);
	}

	// 3) Perform hammering based on rh_type and hammers_per_round
	auto t_start_hammering = chrono::high_resolution_clock::now();

	if (!use_single_softmc_prog)
		hammer_hrs(platform, hammerable_rows, hammers_per_round, cascaded_hammer,
			   num_rounds, skip_hammering_aggr | ignore_aggrs, ignore_dummy_hammers,
			   hammer_duration, num_refs_per_round, pre_ref_delay, dummy_aggrs,
			   dummy_aggrs_bank, hammer_dummies_first, hammer_dummies_independently,
			   num_bank0_hammers);
	else {
		std::string lbl_hammer_all = createSMCLabel("HAMMER_ALL");
		std::string lbl_hammer_end = createSMCLabel("HAMMER_END");
		if (first_it_aggr_init_and_hammer) {
			SMC_REG reg_zero = single_prog_reg_alloc.allocate_SMC_REG();
			single_prog.add_inst(SMC_LI(0, reg_zero));

			single_prog.add_branch(Program::BR_TYPE::BEQ, reg_iter_counter, reg_zero,
					       lbl_hammer_all);
			single_prog_reg_alloc.free_SMC_REG(reg_zero);

			hammer_hrs(platform, hammerable_rows, hammers_per_round, cascaded_hammer,
				   num_rounds, true /*skip_hammering_aggr | ignore_aggrs*/,
				   ignore_dummy_hammers, hammer_duration, num_refs_per_round,
				   pre_ref_delay, dummy_aggrs, dummy_aggrs_bank,
				   hammer_dummies_first, hammer_dummies_independently,
				   num_bank0_hammers, &single_prog, &single_prog_reg_alloc);

			single_prog.add_branch(Program::BR_TYPE::JUMP, reg_iter_counter, reg_zero,
					       lbl_hammer_end);
		}

		single_prog.add_label(lbl_hammer_all);

		hammer_hrs(platform, hammerable_rows, hammers_per_round, cascaded_hammer,
			   num_rounds, skip_hammering_aggr | ignore_aggrs, ignore_dummy_hammers,
			   hammer_duration, num_refs_per_round, pre_ref_delay, dummy_aggrs,
			   dummy_aggrs_bank, hammer_dummies_first, hammer_dummies_independently,
			   num_bank0_hammers, &single_prog, &single_prog_reg_alloc);

		single_prog.add_label(lbl_hammer_end);
	}

	auto t_end_hammering = chrono::high_resolution_clock::now();
	chrono::duration<double, milli> dur_hammering(t_end_hammering - t_start_hammering);
	chrono::duration<double, milli> dur_from_start(t_end_hammering - t_end_issue_prog);

	// 5) wait until the retention time of the weak rows is satisfied (ret_ms * H_MODIFIER)

	if (skip_hammering_aggr) { // no need to wait and read any rows back if we are going to skip
				   // operating on the weak rows
		// just returning an empty vector
		return {};
	}

	if (!use_single_softmc_prog)
		waitMS(hammerable_rows[0].ret_ms * TRR_RETTIME_MULT - dur_from_start.count());
	else
		// we cannot use the measured time interval 'dur_from_start' when executing the
		// experiment as a single program Therefore, we use the calculated time here
		waitMS_softmc(c_wait_interval_ms, &single_prog);

	// 6) read back the weak rows and check for bitflips
	Program *prog_read = nullptr;
	SoftMCRegAllocator *reg_alloc = nullptr;

	bool exec_prog_and_clean = false;
	if (!use_single_softmc_prog) {
		prog_read = new Program();
		reg_alloc = new SoftMCRegAllocator(NUM_SOFTMC_REGS, reserved_regs);
		exec_prog_and_clean = true;
	} else {
		prog_read = &single_prog;
		reg_alloc = &single_prog_reg_alloc;
	}

	SMC_REG reg_bank_addr = reg_alloc->allocate_SMC_REG();
	SMC_REG reg_num_cols = reg_alloc->allocate_SMC_REG();
	if (exec_prog_and_clean)
		add_op_with_delay(*prog_read, SMC_PRE(reg_bank_addr, 0, 1), 0, 0); // precharge all
										   // banks

	prog_read->add_inst(SMC_LI(NUM_COLS_PER_ROW * 8, reg_num_cols));

	ulong total_victim_rows = 0;
	for (auto &hrs : hammerable_rows) {
		prog_read->add_inst(SMC_LI(hrs.bank_id, reg_bank_addr));
		auto rows_to_read = hrs.victim_ids;
		rows_to_read.insert(rows_to_read.end(), hrs.uni_ids.begin(), hrs.uni_ids.end());

		read_row_data(*prog_read, *reg_alloc, reg_bank_addr, reg_num_cols, rows_to_read);
		total_victim_rows += rows_to_read.size();
	}

	if (exec_prog_and_clean) {
		prog_read->add_inst(SMC_END());
		platform.execute(*prog_read);
		delete prog_read;
		delete reg_alloc;
	} else {
		assert(prog_read == &single_prog);

		// close the iteration loop
		single_prog.add_inst(SMC_ADDI(reg_iter_counter, 1, reg_iter_counter));
		single_prog.add_branch(single_prog.BR_TYPE::BL, reg_iter_counter, reg_num_iters,
				       lbl_iter_loop);

		single_prog.add_inst(SMC_END());
		platform.execute(single_prog);

		single_prog_reg_alloc.free_SMC_REG(reg_iter_counter);
		single_prog_reg_alloc.free_SMC_REG(reg_num_iters);
	}

	vector<vector<uint> > loc_bitflips;
	if (!use_single_softmc_prog) {
		// get data from PCIe
		ulong read_data_size = ROW_SIZE_BYTES * total_victim_rows;
		char buf[read_data_size * 2];
		platform.receiveData(buf, read_data_size);

		vector<uint> bitflips;
		loc_bitflips.reserve(total_victim_rows);

		uint row_it = 0;
		for (auto &hrs : hammerable_rows) {
			for (uint vict_ind = 0; vict_ind < hrs.victim_ids.size(); vict_ind++) {
				bitflips = detectSpecificBitflips(buf + row_it * ROW_SIZE_BYTES,
								  ROW_SIZE_BYTES, hrs.data_pattern,
								  hrs.vict_bitflip_locs[vict_ind]);
				row_it++;

				loc_bitflips.push_back(bitflips);
			}

			for (uint uni_ind = 0; uni_ind < hrs.uni_ids.size(); uni_ind++) {
				bitflips = detectSpecificBitflips(buf + row_it * ROW_SIZE_BYTES,
								  ROW_SIZE_BYTES, hrs.data_pattern,
								  hrs.uni_bitflip_locs[uni_ind]);
				row_it++;

				loc_bitflips.push_back(bitflips);
			}
		}
		assert(row_it == total_victim_rows);
	}

	return loc_bitflips;
}

// Finds and returns a subset of the retention-profiled rows in rowGroup that match the provided
// row_layout
RowGroup adjustRowGroup(const RowGroup &rowGroup, const std::string &row_layout)
{
	// calculate row_layout distance vector
	std::vector<uint> wrs_type_dists;
	wrs_type_dists.reserve(row_layout.size());

	int first_r_ind = -1;
	for (uint i = 0; i < row_layout.size(); i++) {
		switch (row_layout[i]) {
		case 'u':
		case 'U':
		case 'r':
		case 'R': {
			if (first_r_ind == -1) {
				first_r_ind = i;
				break;
			}

			wrs_type_dists.push_back(i - first_r_ind);
			break;
		}
		}
	}
	assert(first_r_ind != -1 &&
	       "ERROR: There must be at least one R or U in the rowlayout (i.e., row_layout)");

	RowGroup new_wrs = rowGroup;
	bool match = false;

	std::vector<uint> wrs_dists;
	wrs_dists.reserve(new_wrs.rows.size());
	while (true) {
		// finding the row distance between the rows in rowGroup
		wrs_dists.clear();

		uint first_victim_id = to_physical_row_id(new_wrs.rows[0].row_id);
		for (uint i = 1; i < new_wrs.rows.size(); i++)
			wrs_dists.push_back(to_physical_row_id(new_wrs.rows[i].row_id) -
					    first_victim_id);
		// for RRRRR, wrs_dists would be 1, 2, 3, 4
		// RARAR should match it, which would have dist vector 2 4
		// we should pick weak rows 0, 2, and 4

		if (wrs_dists.size() < wrs_type_dists.size())
			break;

		// the distance vectors are sorted so we can use includes() to determine if
		// wrs_type_dists is a subset of wrs_dists
		match = std::includes(wrs_dists.begin(), wrs_dists.end(), wrs_type_dists.begin(),
				      wrs_type_dists.end());

		if (match)
			break;

		new_wrs.rows.erase(new_wrs.rows.begin()); // remove the first row id and
							  // check if the remaining match
	}

	assert(!wrs_dists.empty() || match); // does includes() return true when wrs_type_dists
					     // is empty?

	if (match) { // we have a match
		// build a new row id vector that includes only the matching rows

		std::vector<Row> matching_weak_rows;
		matching_weak_rows.reserve(wrs_type_dists.size());
		matching_weak_rows.push_back(new_wrs.rows[0]);

		uint new_wrs_it = 0;
		for (auto d : wrs_type_dists) { // evict the rows that do not match the dist vector
			for (; new_wrs_it < wrs_dists.size(); new_wrs_it++) {
				assert(wrs_dists[new_wrs_it] <= d);
				if (wrs_dists[new_wrs_it] == d) { // found a match
					matching_weak_rows.push_back(new_wrs.rows[new_wrs_it + 1]);
					break;
				}
			}
		}

		new_wrs.rows = matching_weak_rows;

		return new_wrs;
	}

	std::cerr << RED_TXT
		  << "ERROR: The provided --row_layout does not match the data read from "
		     "--file_weaks"
		  << NORMAL_TXT << std::endl;
	std::cerr << RED_TXT << "--row_layout: " << row_layout << NORMAL_TXT << std::endl;
	std::cerr << RED_TXT << "Exiting..." << NORMAL_TXT << std::endl;
	exit(-1);

	return new_wrs;
}

void pick_hammerable_row_groups_from_file(SoftMCPlatform &platform, vector<RowGroup> &allRowGroups,
					  vector<RowGroup> &rowGroups, const uint num_row_groups,
					  const bool cascaded_hammer, const std::string &row_layout)
{
	while (rowGroups.size() != num_row_groups) {
		// 1) Pick (in order) 'num_weaks' weak rows from 'file_weak_rows' that have the same
		// retention time.
		rowGroups = selectRowGroupsWithMaxRetTimeDiff(allRowGroups, num_row_groups,
							      TRR_ALLOWED_RET_TIME_DIFF);

		// 2) test whether RowHammer bitflips can be induced on the weak rows
		for (auto rowGroup = rowGroups.begin(); rowGroup != rowGroups.end(); rowGroup++) {
			if (!is_hammerable(platform, *rowGroup, row_layout, cascaded_hammer)) {
				std::cout << RED_TXT << "Candidate victim row set "
					  << rowGroup->toString() << " is not hammerable"
					  << NORMAL_TXT << std::endl;
				rowGroups.erase(rowGroup--);
				continue;
			}

			std::cout << GREEN_TXT << "Candidate victim row " << rowGroup->toString()
				  << " is hammerable" << NORMAL_TXT << std::endl;

			*rowGroup = adjustRowGroup(*rowGroup, row_layout);
		}
	}
}

void get_row_groups_by_index(const vector<RowGroup> &all_row_groups, vector<RowGroup> &row_groups,
			     const vector<uint> &indices, const std::string &row_layout)
{
	// Check all indices are valid before processing
	for (auto index : indices) {
		if (index >= all_row_groups.size()) {
			throw std::out_of_range("ERROR: Index out of range. Requested index " +
						std::to_string(index) +
						" exceeds available row groups count of " +
						std::to_string(all_row_groups.size()) +
						". Please ensure the requested indices are within "
						"the valid range.");
		}
	}

	// If all indices are valid, proceed to adjust and copy the row groups
	for (auto index : indices) {
		const auto &rowGroup = all_row_groups[index];
		auto adjustedRowGroup = adjustRowGroup(rowGroup, row_layout);
		row_groups.push_back(adjustedRowGroup);
	}
}

bool check_dummy_vs_rg_collision(const std::vector<uint> &dummy_aggrs,
				 const std::vector<RowGroup> &vec_wrs)
{
	for (uint dummy : dummy_aggrs) {
		for (const auto &wrs : vec_wrs) {
			for (const auto &weak_row : wrs.rows) {
				if (dummy == weak_row.row_id)
					return true;
			}
		}
	}

	return false;
}

void adjust_hammers_per_ref(std::vector<uint> &hammers_per_round, const uint num_aggrs_in_wrs,
			    const bool hammer_rgs_individually, const bool skip_hammering_aggr,
			    const uint num_wrs, const uint total_aggrs,
			    const std::vector<uint> &dummy_aggr_ids,
			    const uint dummy_hammers_per_round, const bool hammer_dummies_first)
{
	if (hammers_per_round.size() == 1) { // if a single hammers_per_round is specified, hammer
					     // all aggressors in a WRS the same amount
		while (hammers_per_round.size() != num_aggrs_in_wrs)
			hammers_per_round.push_back(hammers_per_round[0]);
	}

	if (!hammer_rgs_individually) { // use the same hammer counts for all WRSs
		uint num_specified_hammers = hammers_per_round.size();

		assert(num_specified_hammers == num_aggrs_in_wrs &&
		       "ERROR: --hammers_per_round must specify exactly one hammer count for each "
		       "aggressor in one WRS unless --hammer_rgs_individually is used to specify "
		       "hammer counts for all WRS");

		for (uint i = 1; i < num_wrs; i++) {
			for (uint j = 0; j < num_specified_hammers; j++)
				hammers_per_round.push_back(hammers_per_round[j]);
		}
	}

	if (hammers_per_round.size() != total_aggrs) {
		std::cerr << RED_TXT << "ERROR: " << hammers_per_round.size()
			  << " hammers per ref specified for " << total_aggrs << " total aggressors"
			  << NORMAL_TXT << std::endl;
		exit(-1);
	}

	for (auto &dummy_id : dummy_aggr_ids) {
		if (hammer_dummies_first)
			hammers_per_round.insert(hammers_per_round.begin(),
						 dummy_hammers_per_round); // insert at front
		else
			hammers_per_round.push_back(dummy_hammers_per_round);
	}
}

int main(int argc, char **argv)
{
	auto config = CLIConfig::parseCommandLine(argc, argv);

	/* Program options */
	auto out_filename = config.output.out_filename;

	uint num_row_groups = config.row_analysis.num_row_groups;
	string row_scout_file = config.row_analysis.row_scout_file;
	string row_layout = config.row_analysis.row_layout;
	vector<uint> row_group_indices = config.row_analysis.row_group_indices;

	vector<uint> hammers_per_round = config.hammer.hammers_per_round;
	vector<uint> hammers_before_wait = config.hammer.hammers_before_wait;
	uint hammer_duration = config.hammer.hammer_duration; // as DDR cycles (1.5ns)
	bool first_it_aggr_init_and_hammer = config.hammer.first_it_aggr_init_and_hammer;

	float init_to_hammerbw_delay = 0.0f;
	bool first_it_dummy_hammer = false;
	uint num_bank0_hammers = 0;
	uint num_pre_init_bank0_hammers = 0;
	uint pre_init_nops = 0;
	uint num_dummy_aggressors = 0;
	int dummy_aggrs_bank = -1;
	uint dummy_hammers_per_round = 1;
	uint dummy_ids_offset = 0;
	vector<uint> arg_dummy_aggr_ids;
	bool hammer_dummies_first = false;
	bool hammer_dummies_independently = false;
	uint num_refs_per_round = 1;
	uint pre_ref_delay = 0;
	bool append_output = false;

	bool only_pick_rgs = false;

	uint refs_after_init = 0;
	uint num_dummy_after_init = 0;
	bool refs_after_init_no_dummy_hammer = false;


	bool use_single_softmc_prog = false;
	bool location_out = false;

	uint arg_log_phys_conv_scheme = 0;

	options_description desc("TRR Analyzer Options");
	desc.add_options()("help,h", "Prints this usage statement.")
		// refresh related args
		("refs_per_round", value(&num_refs_per_round)->default_value(num_refs_per_round),
		 "Specifies how many REF commands to issue at the end of a round, i.e., after "
		 "hammering.")("refs_after_init", value(&refs_after_init),
			       "Specifies the number of REF commands to issue right after "
			       "initializing data in DRAM rows.")

		// dummy row related args
		("num_dummy_aggrs",
		 value(&num_dummy_aggressors)->default_value(num_dummy_aggressors),
		 "Specifies the number of dummy aggressors to hammer in each round. The dummy row "
		 "addresses are selected such that they are different and in safe distance from "
		 "the actual aggressor rows.")(
			"dummy_aggrs_bank",
			value(&dummy_aggrs_bank)->default_value(dummy_aggrs_bank),
			"Specifies the bank address from which dummy rows should be selected. If "
			"not specified, TRR Analyzer picks dummy rows from the same bank as the "
			"row groups.")("dummy_aggr_ids",
				       value<vector<uint> >(&arg_dummy_aggr_ids)->multitoken(),
				       "Specifies the exact dummy row addresses to hammer in each "
				       "round instead of letting TRR Analyzer select the dummy "
				       "rows.")(
			"dummy_hammers_per_round",
			value(&dummy_hammers_per_round)->default_value(dummy_hammers_per_round),
			"Specifies how many times each dummy row to hammer in each round.")(
			"dummy_ids_offset",
			value(&dummy_ids_offset)->default_value(dummy_ids_offset),
			"Specifies a value to offset every dummy row address. Useful when there is "
			"a need to pick different dummy rows in different runs of TRR Analyzer.")(
			"hammer_dummies_first", bool_switch(&hammer_dummies_first),
			"When specified, the dummy rows are hammered before hammering the actual "
			"aggressor rows.")("hammer_dummies_independently",
					   bool_switch(&hammer_dummies_independently),
					   "When specified, the dummy rows are hammered after the "
					   "aggressor rows to matter whether --cascaded is used or "
					   "not. The dummy rows are simply treated as a separate "
					   "group of rows to hammer after hammering the aggressor "
					   "rows in interleaved or cascaded way.")(
			"num_dummy_after_init",
			value(&num_dummy_after_init)->default_value(num_dummy_after_init),
			"Specifies the number of dummy rows to hammer right after initializing the "
			"victim and aggressor rows. These dummy row hammers happen concurrently "
			"with --refs_after_init refreshes. Each dummy is hammered as much as "
			"possible based on the refresh interval and --refs_after_init.")(
			"refs_after_init_no_dummy_hammer",
			bool_switch(&refs_after_init_no_dummy_hammer),
			"When specified, after hammering dummy rows as specified by "
			"--num_dummy_after_init, TRR Analyzer also performs another set of "
			"refreshes but this time without hammering dummy rows.")(
			"first_it_dummy_hammer", bool_switch(&first_it_dummy_hammer),
			"When specified, the dummy rows are hammered only during the first "
			"iteration.")

		// other. args
		("init_to_hammerbw_delay",
		 value(&init_to_hammerbw_delay)->default_value(init_to_hammerbw_delay),
		 "A float in range [0,1] that specifies the ratio of time to wait before "
		 "performing --hammers_before_wait. The default value (0) means all the delay is "
		 "inserted after performing --hammers_before_wait (if specified)")(
			"num_bank0_hammers",
			value(&num_bank0_hammers)->default_value(num_bank0_hammers),
			"Specifies how many times a row from bank 0 should be hammered after "
			"hammering the aggressor and dummy rows.")(
			"num_pre_init_bank0_hammers",
			value(&num_pre_init_bank0_hammers)
				->default_value(num_pre_init_bank0_hammers),
			"Specifies how many times a row from bank 0 should be hammered before "
			"initializing data in victim and aggressor rows.")(
			"pre_init_nops", value(&pre_init_nops)->default_value(pre_init_nops),
			"Specifies the number of NOPs (as FPGA cycles, i.e., 4 DRAM cycles) to be "
			"inserted before victim/aggressor data initialization.")(
			"pre_ref_delay", value(&pre_ref_delay)->default_value(pre_ref_delay),
			"Specifies the number of cycles to wait before performing REFs specified "
			"by --refs_per_round. Must be 8 or larger if not 0 for this arg to take an "
			"effect.")("only_pick_rgs", bool_switch(&only_pick_rgs),
				   "When specified, the test finds hammerable row groups rows in "
				   "--row_scout_file, but it does not run the TRR analysis.")(
			"log_phys_scheme",
			value(&arg_log_phys_conv_scheme)->default_value(arg_log_phys_conv_scheme),
			"Specifies how to convert logical row IDs to physical row ids and the "
			"other way around. Pass 0 (default) for sequential mapping, 1 for the "
			"mapping scheme typically used in Samsung chips.")(
			"use_single_softmc_prog", bool_switch(&use_single_softmc_prog),
			"When specified, the entire experiment executes as a single SoftMC "
			"program. This is to prevent SoftMC maintenance operations to kick in "
			"between multiple SoftMC programs. However, using this option may result "
			"in a very large program that may exceed the instruction limit.")(
			"append", bool_switch(&append_output),
			"When specified, the output of TRR Analyzer is appended to the --out file. "
			"Otherwise the --out file is cleared.")("location_out",
								bool_switch(&location_out),
								"When specified, the bit flip "
								"locations are written to the "
								"--out file.");

	variables_map vm;
	boost::program_options::store(parse_command_line(argc, argv, desc), vm);
	if (vm.count("help")) {
		cout << desc << endl;
		return 0;
	}

	notify(vm);

	if (!row_group_indices.empty())
		num_row_groups = row_group_indices.size();

	if (!arg_dummy_aggr_ids.empty()) {
		num_dummy_aggressors = arg_dummy_aggr_ids.size();
	}

	if (row_layout.empty()) {
		auto dot_pos = row_scout_file.find_last_of('.');
		if (dot_pos != string::npos)
			row_layout = row_scout_file.substr(dot_pos + 1);
		else {
			std::cerr << RED_TXT
				  << "ERROR: Could not find '.' in the provided --row_scout_file\n"
				  << std::endl;
			exit(-5);
		}
	}

	if (!std::regex_match(row_layout, std::regex("^[RrAaUu-]+$"))) {
		std::cerr << RED_TXT
			  << "ERROR: --row_layout should contain only 'R', 'A', 'U', and '-' "
			     "characters. Provided: "
			  << row_layout << NORMAL_TXT << std::endl;
		exit(-3);
	}

	if (!out_filename.empty()) {
		path out_dir(out_filename);
		out_dir = out_dir.parent_path();
		if (!(exists(out_dir))) {
			if (!create_directory(out_dir)) {
				cerr << "Cannot create directory: " << out_dir << ". Exiting..."
				     << endl;
				return -1;
			}
		}
	}

	boost::filesystem::ofstream out_file;
	if (!out_filename.empty()) {
		if (append_output)
			out_file.open(out_filename, boost::filesystem::ofstream::app);
		else
			out_file.open(out_filename);
	} else {
		out_file.open("/dev/null");
	}

	SoftMCPlatform platform;
	int err;

	if ((err = platform.init()) != SOFTMC_SUCCESS) {
		cerr << "Could not initialize SoftMC Platform: " << err << endl;
		return err;
	}

	platform.reset_fpga();
	platform.set_aref(false); // disable refresh

	assert(arg_log_phys_conv_scheme < uint(LogPhysRowIDScheme::MAX));
	logical_physical_conversion_scheme = (LogPhysRowIDScheme)arg_log_phys_conv_scheme;

	// init random data generator
	std::srand(0);

	bitset<512> bitset_int_mask(0xFFFFFFFF);

	auto t_prog_started = chrono::high_resolution_clock::now();
	chrono::duration<double> elapsed{};
	bool check_time;

	vector<uint> picked_weak_indices;
	picked_weak_indices.reserve(num_row_groups);

	auto allRowGroups = parseAllRowGroups(row_scout_file);

	vector<RowGroup> row_groups;
	row_groups.reserve(num_row_groups);
	if (!row_group_indices.empty()) {
		get_row_groups_by_index(allRowGroups, row_groups, row_group_indices, row_layout);
	} else if (num_row_groups > 0) {
		pick_hammerable_row_groups_from_file(platform, allRowGroups, row_groups,
						     num_row_groups, config.hammer.cascaded_hammer,
						     row_layout);
	}

	if (only_pick_rgs) { // write the picked weak row indices to the output file and exit
		for (auto &rg : row_groups)
			out_file << rg.index_in_file << " ";

		return 0;
	}

	if (dummy_aggrs_bank == -1)
		dummy_aggrs_bank = row_groups[0].bank_id;

	// 3) Pick dummy aggressors rows
	if ((num_dummy_aggressors > 0) && arg_dummy_aggr_ids.empty()) {
		uint max_dummy_aggrs = num_dummy_aggressors;
		arg_dummy_aggr_ids.reserve(max_dummy_aggrs);
		pick_dummy_aggressors(arg_dummy_aggr_ids, dummy_aggrs_bank, max_dummy_aggrs,
				      row_groups, dummy_ids_offset);

	} else if (!arg_dummy_aggr_ids.empty() &&
		   (dummy_aggrs_bank == row_groups[0].bank_id)) { // check whether the user provided
								  // dummy row ids collide with the
								  // aggressor row ids
		if (check_dummy_vs_rg_collision(arg_dummy_aggr_ids, row_groups)) {
			std::cerr << RED_TXT
				  << "ERROR: The user provided dummy aggressor rows collide with "
				     "victims/aggressor rows. Finishing the test!"
				  << NORMAL_TXT << std::endl;
			return -2;
		}
	}

	std::cout << YELLOW_TXT << "Dummy rows (while hammering): " << std::endl;
	for (auto dummy : arg_dummy_aggr_ids) {
		std::cout << dummy << " ";
	}
	std::cout << std::endl;

	// pick dummy rows that are hammered right after initializing data while performing refresh
	// operations
	std::vector<uint> after_init_dummies;
	if (num_dummy_after_init > 0) {
		std::vector<RowGroup> cur_rgs_and_dummies = row_groups;
		RowGroup cur_dummies;

		// this is to pick different dummy than those we picked to hammer while hammering
		// the actual aggressor rows
		for (uint dummy_row_id : arg_dummy_aggr_ids)
			cur_dummies.rows.emplace_back(dummy_row_id, std::vector<uint>());
		cur_rgs_and_dummies.push_back(cur_dummies);

		pick_dummy_aggressors(after_init_dummies, dummy_aggrs_bank, num_dummy_after_init,
				      cur_rgs_and_dummies, dummy_ids_offset);
	}

	// 4) Perform TRR analysis for each position of the weak rows among the dummy rows

	// convert row groups to hammerable row set
	std::vector<HammerableRowSet> hrs;
	hrs.reserve(row_groups.size());

	uint total_victims = 0;
	uint total_aggrs = 0;
	for (auto &wrs : row_groups) {
		hrs.push_back(toHammerableRowSet(wrs, row_layout));
		total_victims += hrs.back().victim_ids.size();
		total_aggrs += hrs.back().aggr_ids.size();
	}

	auto aggr_hammers_per_ref = hammers_per_round;

	if (total_aggrs > 0)
		adjust_hammers_per_ref(hammers_per_round, hrs[0].aggr_ids.size(),
				       config.hammer.hammer_rgs_individually,
				       config.hammer.skip_hammering_aggr, row_groups.size(),
				       total_aggrs, arg_dummy_aggr_ids, dummy_hammers_per_round,
				       hammer_dummies_first);

	if (!hammers_before_wait.empty())
		adjust_hammers_per_ref(hammers_before_wait, hrs[0].aggr_ids.size(),
				       config.hammer.hammer_rgs_individually,
				       config.hammer.skip_hammering_aggr, row_groups.size(),
				       total_aggrs, arg_dummy_aggr_ids, 0, hammer_dummies_first);

	vector<uint> total_bitflips(total_victims, 0);

	// Setting up a progress bar
	progresscpp::ProgressBar progress_bar(config.experiment.num_iterations, 70, '#', '-');

	std::cout << BLUE_TXT << "Num hammerable row sets: " << hrs.size() << NORMAL_TXT
		  << std::endl;
	uint hr_ind = 0;
	uint hammers_ind = 0;
	for (auto hr : hrs) {
		std::cout << BLUE_TXT << "Hammerable row set " << hr_ind << NORMAL_TXT << std::endl;

		std::cout << BLUE_TXT << "Victims: ";
		for (auto vict_id : hrs[hr_ind].victim_ids) {
			std::cout << vict_id << ", ";
		}
		std::cout << NORMAL_TXT << std::endl;

		std::cout << BLUE_TXT << "Aggressors: ";
		for (auto aggr_id : hrs[hr_ind].aggr_ids) {
			std::cout << aggr_id << " (";
			std::cout << aggr_hammers_per_ref[hammers_ind++] << "), ";
		}
		std::cout << NORMAL_TXT << std::endl;

		std::cout << BLUE_TXT << "Unified Rows: ";
		for (auto uni_id : hrs[hr_ind].uni_ids) {
			std::cout << uni_id << " (";
			std::cout << aggr_hammers_per_ref[hammers_ind++] << "), ";
		}
		std::cout << NORMAL_TXT << std::endl;

		hr_ind++;
	}

	std::cout << BLUE_TXT << "tRAS: " << tras_cycles << " cycles" << NORMAL_TXT << std::endl;

	// printing experiment parameters
	out_file << "row_layout=" << row_layout << std::endl;
	out_file << "--- END OF HEADER ---" << std::endl;

	if (!use_single_softmc_prog) {
		for (uint i = 0; i < config.experiment.num_iterations; i++) {
			bool ignore_aggrs = first_it_aggr_init_and_hammer && i != 0;
			bool ignore_dummy_hammers = first_it_dummy_hammer && i != 0;
			bool verbose = (i == 0);
			auto loc_bitflips = analyzeTRR(
				platform, hrs, arg_dummy_aggr_ids, dummy_aggrs_bank,
				dummy_hammers_per_round, hammer_dummies_first,
				hammer_dummies_independently, config.hammer.cascaded_hammer,
				hammers_per_round, config.hammer.hammer_cycle_time, hammer_duration,
				config.experiment.num_rounds, config.hammer.skip_hammering_aggr,
				refs_after_init, after_init_dummies, config.hammer.init_aggrs_first, ignore_aggrs, config.hammer.init_only_victims, ignore_dummy_hammers,
				first_it_aggr_init_and_hammer, refs_after_init_no_dummy_hammer,
				num_refs_per_round, pre_ref_delay, hammers_before_wait,
				init_to_hammerbw_delay, num_bank0_hammers,
				num_pre_init_bank0_hammers, pre_init_nops, false, 0, verbose);

			++progress_bar;
			progress_bar.display();

			out_file << "Iteration " << i << " bitflips:" << std::endl;

			if (!config.hammer.skip_hammering_aggr) {
				uint bitflips_ind = 0;

				for (auto &hr : hrs) {
					uint total_rows = hr.victim_ids.size() + hr.uni_ids.size();

					std::vector<std::string> output_strs_vict, output_strs_uni;

					for (uint vict : hr.victim_ids) {
						// out_file << "Victim row " << vict << ": " <<
						// num_bitflips[it_vict] << std::endl;
						string output_str_vict;
						output_str_vict =
							"Victim row " + to_string(vict) + ": " +
							to_string(
								loc_bitflips[bitflips_ind].size());
						if (location_out) {
							output_str_vict += ": ";
							for (auto loc : loc_bitflips[bitflips_ind])
								output_str_vict +=
									to_string(loc) + ", ";
						}
						output_strs_vict.push_back(output_str_vict);
						total_bitflips[bitflips_ind] +=
							loc_bitflips[bitflips_ind].size();
						bitflips_ind++;
					}

					for (uint uni : hr.uni_ids) {
						// out_file << "Victim row(U) " << uni << ": " <<
						// num_bitflips[it_vict] << std::endl;
						string output_str_uni;
						output_str_uni =
							"Victim row(U) " + to_string(uni) + ": " +
							to_string(
								loc_bitflips[bitflips_ind].size());
						if (location_out) {
							output_str_uni += ": ";
							for (auto loc : loc_bitflips[bitflips_ind])
								output_str_uni +=
									to_string(loc) + ", ";
						}
						output_strs_uni.push_back(output_str_uni);
						total_bitflips[bitflips_ind] +=
							loc_bitflips[bitflips_ind].size();
						bitflips_ind++;
					}

					// reordering rows based on their physical row IDs
					uint uni_ind = 0;
					for (uint vict_ind = 0; vict_ind < hr.victim_ids.size();
					     vict_ind++) {
						if (uni_ind != hr.uni_ids.size() &&
						    to_physical_row_id(hr.uni_ids[uni_ind]) <
							    to_physical_row_id(
								    hr.victim_ids[vict_ind])) {
							out_file << output_strs_uni[uni_ind++]
								 << std::endl;
							vict_ind--;
						} else {
							out_file << output_strs_vict[vict_ind]
								 << std::endl;
						}
					}

					for (; uni_ind < hr.uni_ids.size(); uni_ind++)
						out_file << output_strs_uni[uni_ind] << std::endl;
				}
			}
		}
	} else {
		assert(!first_it_dummy_hammer &&
		       "ERROR: --first_it_dummy_hammer is not yet supported when running the "
		       "experiments as a single SoftMC program.");
		// run the experiment as a single SoftMC program
		auto num_bitflips = analyzeTRR(
			platform, hrs, arg_dummy_aggr_ids, dummy_aggrs_bank,
			dummy_hammers_per_round, hammer_dummies_first, hammer_dummies_independently,
			config.hammer.cascaded_hammer, hammers_per_round,
			config.hammer.hammer_cycle_time,
			hammer_duration, config.experiment.num_rounds,
			config.hammer.skip_hammering_aggr, refs_after_init, after_init_dummies, config.hammer.init_aggrs_first, false,
			config.hammer.init_only_victims, false,
			first_it_aggr_init_and_hammer, refs_after_init_no_dummy_hammer,
			num_refs_per_round, pre_ref_delay, hammers_before_wait,
			init_to_hammerbw_delay, num_bank0_hammers, num_pre_init_bank0_hammers,
			pre_init_nops, true, config.experiment.num_iterations, true);

		// num_bitflips contains nothing since we have not read data from the PCIe yet
		// receive PCIe data iteration by iteration and keep the out_file format the same

		ulong read_data_size = ROW_SIZE_BYTES * total_victims;
		char *buf = new char[read_data_size];
		vector<uint> bitflips;

		for (uint i = 0; i < config.experiment.num_iterations; i++) {
			if (!config.hammer.skip_hammering_aggr) {
				platform.receiveData(buf, read_data_size);

				out_file << "Iteration " << i << " bitflips:" << std::endl;

				uint row_it = 0;
				for (auto &hr : hrs) {
					for (uint vict_ind = 0; vict_ind < hr.victim_ids.size();
					     vict_ind++) {
						bitflips = detectSpecificBitflips(
							buf + row_it * ROW_SIZE_BYTES,
							ROW_SIZE_BYTES, hr.data_pattern,
							hr.vict_bitflip_locs[vict_ind]);
						row_it++;

						out_file << "Victim row " << hr.victim_ids[vict_ind]
							 << ": " << bitflips.size();
						if (location_out) {
							out_file << ": ";
							for (auto loc : bitflips)
								out_file << loc << ", ";
						}
						out_file << endl;
						total_bitflips[row_it] += bitflips.size();
					}

					auto aggr_data_pattern = hr.data_pattern;
					aggr_data_pattern.flip();
					for (uint uni_ind = 0; uni_ind < hr.uni_ids.size();
					     uni_ind++) {
						bitflips = detectSpecificBitflips(
							buf + row_it * ROW_SIZE_BYTES,
							ROW_SIZE_BYTES, aggr_data_pattern,
							hr.uni_bitflip_locs[uni_ind]);
						row_it++;

						out_file << "Victim row(U) " << hr.uni_ids[uni_ind]
							 << ": " << bitflips.size();
						if (location_out) {
							out_file << ": ";
							for (auto loc : bitflips)
								out_file << loc << ", ";
						}
						out_file << endl;
						total_bitflips[row_it] += bitflips.size();
					}
				}
				assert(row_it == total_victims);
			}

			++progress_bar;
			progress_bar.display(); // the progress bar is probably not that useful here
		}

		delete[] buf;
	}

	if (!config.hammer.skip_hammering_aggr) {
		out_file << "Total bitflips:" << std::endl;
		uint it_vict = 0;
		for (auto &hr : hrs) {
			for (uint vict : hr.victim_ids) {
				out_file << "Victim row " << vict << ": "
					 << total_bitflips[it_vict++] << std::endl;
			}

			for (uint uni : hr.uni_ids) {
				out_file << "Victim row(U) " << uni << ": "
					 << total_bitflips[it_vict++] << std::endl;
			}
		}
	}

	progress_bar.done();

	std::cout << "The test has finished!" << endl;

	out_file.close();

	return 0;
}