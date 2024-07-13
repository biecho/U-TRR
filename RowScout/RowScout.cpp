#include "ProgressBar.hpp"
#include "instruction.h"
#include "json_struct.h"
#include "platform.h"
#include "prog.h"
#include "softmc_utils.h"

#include <algorithm>
#include <array>
#include <bitset>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <list>
#include <numeric>
#include <string>

#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
using namespace boost::program_options;
using namespace boost::filesystem;

using namespace std;

#define CASR 0
#define BASR 1
#define RASR 2

#define NUM_SOFTMC_REGS 16
#define FPGA_PERIOD 1.5015f // ns

#define RED_TXT "\033[31m"
#define GREEN_TXT "\033[32m"
#define BLUE_TXT "\033[34m"
#define MAGENTA_TXT "\033[35m"
#define NORMAL_TXT "\033[0m"

/*** DRAM Organization Parameters - UPDATE here if the organization of your DRAM module differs ***/
int NUM_BANKS = 16; // this is the total number of banks in the chip
int NUM_BANK_GROUPS = 4;
int NUM_ROWS = 32768;
int ROW_SIZE = 8192;
int NUM_COLS_PER_ROW = 128;
/******/

/*** DRAM Timing Parameters - UPDATE the timing parameters to match the timings of your module ***/
float DEFAULT_TRCD = 13.5f; // ns
float DEFAULT_TRAS = 35.0f; // ns
float DEFAULT_TRP = 13.5f; // ns
float DEFAULT_TWR = 15.0f; // ns
float DEFAULT_TRFC = 260.0f; // ns
float DEFAULT_TRRDS = 5.3f; // ns (ACT-ACT to different bank groups)
float DEFAULT_TRRDL = 6.4f; // ns (ACT-ACT to same bank group)
/******/

int trcd_cycles = (int)ceil(DEFAULT_TRCD / FPGA_PERIOD);
int tras_cycles = (int)ceil(DEFAULT_TRAS / FPGA_PERIOD);
int trp_cycles = (int)ceil(DEFAULT_TRP / FPGA_PERIOD);
int twr_cycles = (int)ceil(DEFAULT_TWR / FPGA_PERIOD);
int trfc_cycles = (int)ceil(DEFAULT_TRFC / FPGA_PERIOD);
int trrds_cycles = (int)ceil(DEFAULT_TRRDS / FPGA_PERIOD);
int trrdl_cycles = (int)ceil(DEFAULT_TRRDL / FPGA_PERIOD);

// Retention Profiler Parameters
uint RETPROF_NUM_ITS = 100; // When a candidate row group is found, the profiler repeats the
			    // retention time test on the on the row num_test_iterations number of
			    // times to make sure the row is reliably weak
float RETPROF_RETTIME_STEP = 1.0f; // defines by how much to increase the target retention time if
				   // sufficient row groups not found in the previous iteration
float RETPROF_RETTIME_MULT_H = 1.2f;

vector<uint32_t> reserved_regs{ CASR, BASR, RASR };

typedef struct RowData {
	bitset<512> input_data_pattern;
	uint pattern_id;
	string label;
} RowData;

typedef struct WeakRow {
	uint row_id;
	std::vector<uint> bitflip_locs;
	WeakRow(uint _row_id, vector<uint> _bitflip_locs)
		: row_id(_row_id)
		, bitflip_locs(_bitflip_locs)
	{
	}
} Row;

JS_OBJECT_EXTERNAL(Row, JS_MEMBER(row_id), JS_MEMBER(bitflip_locs));
// JS_MEMBER(data_pattern_type));

typedef struct RowGroup {
	std::vector<Row> rows;
	uint bank_id;
	uint ret_ms;
	uint data_pattern_type;
	uint rowdata_ind;
	RowGroup(std::vector<Row> _rows, uint _bank_id, uint _ret_ms, uint _data_pattern_type,
		 uint _rowdata_ind)
		: rows(_rows)
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

} RowGroup;

JS_OBJECT_EXTERNAL(RowGroup, JS_MEMBER(rows), JS_MEMBER(bank_id), JS_MEMBER(ret_ms),
		   JS_MEMBER(data_pattern_type));

LogicalRowID to_logical_row_id(uint physical_row_id)
{
	switch (logical_physical_conversion_scheme) {
	case LogPhysRowIDScheme::SEQUENTIAL: {
		return physical_row_id;
	}
	case LogPhysRowIDScheme::SAMSUNG: {
		if (physical_row_id & 0x8) {
			PhysicalRowID log_row_id = physical_row_id & 0xFFFFFFF9;
			log_row_id |= (~physical_row_id & 0x00000006); // set bit pos 3 and 2

			return log_row_id;
		} else {
			return physical_row_id;
		}
	}
	default: {
		std::cerr << "ERROR: unimplemented physical to logical row id conversion scheme!"
			  << std::endl;
	}
	}

	return 0;
}

vector<RowGroup> filterCandidateRowGroups(const vector<RowGroup> &rowGroups,
					  const vector<RowGroup> &candidateRowGroups)
{
	vector<RowGroup> filteredCandidates;

	// Check each candidate row group
	for (const auto &candidate : candidateRowGroups) {
		bool hasCommon = false;

		// Compare against each existing row group
		for (const auto &rowGroup : rowGroups) {
			if (rowGroup.hasCommonRowWith(candidate)) {
				hasCommon = true;
				break; // No need to check further if a common row is found
			}
		}

		// Add to filtered list if no common rows were found
		if (!hasCommon) {
			filteredCandidates.push_back(candidate);
		}
	}

	return filteredCandidates;
}

// returns a vector of bit positions that experienced bitflips
vector<uint> collect_bitflips(const char *read_data, const RowData &rh_row)
{
	vector<uint> bitflips;
	bitset<512> read_data_bitset;

	uint32_t *iread_data = (uint32_t *)read_data;

	// check for bitflips in each cache line
	for (int cl = 0; cl < ROW_SIZE / 64; cl++) {
		read_data_bitset.reset();
		for (int i = 0; i < 512 / 32; i++) {
			bitset<512> tmp_bitset = iread_data[cl * (512 / 32) + i];

			read_data_bitset |= (tmp_bitset << i * 32);
		}

		// compare and print errors
		bitset<512> error_mask = read_data_bitset ^ rh_row.input_data_pattern;

		if (error_mask.any()) {
			// there is at least one bitflip in this cache line

			for (uint i = 0; i < error_mask.size(); i++) {
				if (error_mask.test(i)) {
					bitflips.push_back(cl * CACHE_LINE_BITS + i);
				}
			}
		}
	}

	return bitflips;
}

void writeToDRAM(Program &program, const uint target_bank, const uint start_row,
		 const uint row_batch_size, const vector<RowData> &rows_data)
{
	const int REG_TMP_WRDATA = 15;
	const int REG_BANK_ADDR = 12;
	const int REG_ROW_ADDR = 13;
	const int REG_COL_ADDR = 14;
	const int REG_NUM_COLS = 11;

	const int REG_BATCH_IT = 6;
	const int REG_BATCH_SIZE = 5;

	bitset<512> bitset_int_mask(0xFFFFFFFF);
	int remaining_cycs = 0;

	// ===== BEGIN SoftMC Program =====

	program.add_inst(SMC_LI(start_row, REG_ROW_ADDR));
	program.add_inst(SMC_LI(target_bank, REG_BANK_ADDR));

	add_op_with_delay(program, SMC_PRE(REG_BANK_ADDR, 0, 1), 0, 0); // precharge all banks

	program.add_inst(SMC_LI(NUM_COLS_PER_ROW * 8, REG_NUM_COLS));

	program.add_inst(SMC_LI(8, CASR)); // Load 8 into CASR since each READ reads 8 columns
	program.add_inst(SMC_LI(1, BASR)); // Load 1 into BASR
	program.add_inst(SMC_LI(1, RASR)); // Load 1 into RASR

	/* ==== Initialize data of rows in the batch ==== */
	program.add_inst(SMC_LI(0, REG_BATCH_IT));
	program.add_inst(SMC_LI(row_batch_size, REG_BATCH_SIZE));

	assert(row_batch_size % rows_data.size() == 0 &&
	       "Data patterns to initialize consecutive rows with must be multiple of the batch of "
	       "row to initialize at once.");

	program.add_label("INIT_BATCH");

	int row_it = 0;
	for (auto &row_data : rows_data) {
		// set up the input data in the wide register
		for (int pos = 0; pos < 16; pos++) {
			program.add_inst(SMC_LI(
				(((row_data.input_data_pattern >> 32 * pos) & bitset_int_mask)
					 .to_ulong() &
				 0xFFFFFFFF),
				REG_TMP_WRDATA));
			program.add_inst(SMC_LDWD(REG_TMP_WRDATA, pos));
		}

		remaining_cycs -= (16 * 4 * 2 + 4);
		assert(remaining_cycs <= 0 && "I should add some delay here");

		// activate the next row and increment the row address register
		add_op_with_delay(program, SMC_ACT(REG_BANK_ADDR, 0, REG_ROW_ADDR, 1), 0,
				  trcd_cycles - 1);

		// write data to the row and precharge
		program.add_inst(SMC_LI(0, REG_COL_ADDR));

		string new_lbl = "INIT_ROW" + to_string(row_it++);
		program.add_label(new_lbl);
		add_op_with_delay(program, SMC_WRITE(REG_BANK_ADDR, 0, REG_COL_ADDR, 1, 0, 0), 0,
				  0);
		remaining_cycs = 0;
		program.add_branch(program.BR_TYPE::BL, REG_COL_ADDR, REG_NUM_COLS, new_lbl);

		// Wait for t(write-precharge)
		// & precharge the open bank
		remaining_cycs =
			add_op_with_delay(program, SMC_PRE(REG_BANK_ADDR, 0, 0), 0, trp_cycles);
	}

	program.add_inst(SMC_ADDI(REG_BATCH_IT, rows_data.size(), REG_BATCH_IT));
	program.add_branch(program.BR_TYPE::BL, REG_BATCH_IT, REG_BATCH_SIZE, "INIT_BATCH");

	program.add_inst(SMC_END());
}

void writeToDRAM(Program &program, SoftMCRegAllocator &reg_alloc, const uint target_bank,
		 const RowGroup &wrs, const vector<RowData> &rows_data)
{
	SMC_REG REG_TMP_WRDATA = reg_alloc.allocate_SMC_REG();
	SMC_REG REG_BANK_ADDR = reg_alloc.allocate_SMC_REG();
	SMC_REG REG_ROW_ADDR = reg_alloc.allocate_SMC_REG();
	SMC_REG REG_COL_ADDR = reg_alloc.allocate_SMC_REG();
	SMC_REG REG_NUM_COLS = reg_alloc.allocate_SMC_REG();

	bitset<512> bitset_int_mask(0xFFFFFFFF);

	// ===== BEGIN SoftMC Program =====

	program.add_inst(SMC_LI(target_bank, REG_BANK_ADDR));
	add_op_with_delay(program, SMC_PRE(REG_BANK_ADDR, 0, 1), 0, 0); // precharge all banks
	program.add_inst(SMC_LI(NUM_COLS_PER_ROW * 8, REG_NUM_COLS));

	program.add_inst(SMC_LI(8, CASR)); // Load 8 into CASR since each READ reads 8 columns
	program.add_inst(SMC_LI(1, BASR)); // Load 1 into BASR
	program.add_inst(SMC_LI(1, RASR)); // Load 1 into RASR

	/* ==== Initialize data of rows in the row group ==== */

	// initialize the entire range that corresponds to the psysical row ids according to the
	// row_group_pattern
	PhysicalRowID first_phys_row_id = to_physical_row_id(wrs.rows.front().row_id);
	PhysicalRowID last_phys_row_id = to_physical_row_id(wrs.rows.back().row_id);
	assert(last_phys_row_id >= first_phys_row_id);

	std::vector<LogicalRowID> rows_to_init;
	rows_to_init.reserve(last_phys_row_id - first_phys_row_id + 1);

	for (uint i = first_phys_row_id; i <= last_phys_row_id; i++) {
		rows_to_init.push_back(to_logical_row_id(i));
	}

	// set up the input data in the wide register
	for (int pos = 0; pos < 16; pos++) {
		program.add_inst(
			SMC_LI((((rows_data[wrs.rowdata_ind].input_data_pattern >> 32 * pos) &
				 bitset_int_mask)
					.to_ulong() &
				0xFFFFFFFF),
			       REG_TMP_WRDATA));
		program.add_inst(SMC_LDWD(REG_TMP_WRDATA, pos));
	}

	for (uint row_id : rows_to_init) {
		program.add_inst(SMC_LI(row_id, REG_ROW_ADDR));
		// activate the next row and increment the row address register
		add_op_with_delay(program, SMC_ACT(REG_BANK_ADDR, 0, REG_ROW_ADDR, 1), 0,
				  trcd_cycles - 1);

		// write data to the row and precharge
		program.add_inst(SMC_LI(0, REG_COL_ADDR));

		string new_lbl = createSMCLabel("INIT_ROW_DATA");
		program.add_label(new_lbl);
		add_op_with_delay(program, SMC_WRITE(REG_BANK_ADDR, 0, REG_COL_ADDR, 1, 0, 0), 0,
				  0);
		add_op_with_delay(program, SMC_WRITE(REG_BANK_ADDR, 0, REG_COL_ADDR, 1, 0, 0), 0,
				  0);
		add_op_with_delay(program, SMC_WRITE(REG_BANK_ADDR, 0, REG_COL_ADDR, 1, 0, 0), 0,
				  0);
		add_op_with_delay(program, SMC_WRITE(REG_BANK_ADDR, 0, REG_COL_ADDR, 1, 0, 0), 0,
				  0);
		program.add_branch(program.BR_TYPE::BL, REG_COL_ADDR, REG_NUM_COLS, new_lbl);

		// Wait for t(write-precharge)
		// & precharge the open bank
		add_op_with_delay(program, SMC_PRE(REG_BANK_ADDR, 0, 0), 0, trp_cycles - 1);
	}

	program.add_inst(SMC_END());

	reg_alloc.free_SMC_REG(REG_TMP_WRDATA);
	reg_alloc.free_SMC_REG(REG_BANK_ADDR);
	reg_alloc.free_SMC_REG(REG_ROW_ADDR);
	reg_alloc.free_SMC_REG(REG_COL_ADDR);
	reg_alloc.free_SMC_REG(REG_NUM_COLS);
}

void readFromDRAM(Program &program, const uint target_bank, const uint start_row,
		  const uint row_batch_size)
{
	const int REG_BANK_ADDR = 12;
	const int REG_ROW_ADDR = 13;
	const int REG_COL_ADDR = 14;
	const int REG_NUM_COLS = 11;

	const int REG_BATCH_IT = 6;
	const int REG_BATCH_SIZE = 5;

	int remaining_cycs = 0;

	// ===== BEGIN SoftMC Program =====

	program.add_inst(SMC_LI(start_row, REG_ROW_ADDR));
	program.add_inst(SMC_LI(target_bank, REG_BANK_ADDR));

	add_op_with_delay(program, SMC_PRE(REG_BANK_ADDR, 0, 1), 0, 0); // precharge all banks

	// program.add_inst(SMC_LI(NUM_ROWS, REG_NUM_ROWS));
	program.add_inst(SMC_LI(NUM_COLS_PER_ROW * 8, REG_NUM_COLS));

	program.add_inst(SMC_LI(8, CASR)); // Load 8 into CASR since each READ reads 8 columns
	program.add_inst(SMC_LI(1, BASR)); // Load 1 into BASR
	program.add_inst(SMC_LI(1, RASR)); // Load 1 into RASR

	/* ==== Read the data of rows in the batch ==== */
	program.add_inst(SMC_LI(0, REG_BATCH_IT));
	program.add_inst(SMC_LI(row_batch_size, REG_BATCH_SIZE));

	program.add_label("READ_BATCH");

	// activate the next row and increment the row address register
	add_op_with_delay(program, SMC_ACT(REG_BANK_ADDR, 0, REG_ROW_ADDR, 1), 0, trcd_cycles - 1);

	// issue read cmds to read out the entire row and precharge
	program.add_inst(SMC_LI(0, REG_COL_ADDR));

	string new_lbl = "READ_ROW";
	program.add_label(new_lbl);
	add_op_with_delay(program, SMC_READ(REG_BANK_ADDR, 0, REG_COL_ADDR, 1, 0, 0),
			  remaining_cycs, 4);
	remaining_cycs = 0;
	program.add_branch(program.BR_TYPE::BL, REG_COL_ADDR, REG_NUM_COLS, new_lbl);

	// Wait for t(write-precharge)
	// & precharge the open bank
	remaining_cycs = add_op_with_delay(program, SMC_PRE(REG_BANK_ADDR, 0, 0), 0, trp_cycles);

	program.add_inst(SMC_ADDI(REG_BATCH_IT, 1, REG_BATCH_IT));
	program.add_branch(program.BR_TYPE::BL, REG_BATCH_IT, REG_BATCH_SIZE, "READ_BATCH");

	program.add_inst(SMC_END());
}

void readFromDRAM(Program &program, const uint target_bank, const RowGroup &wrs)
{
	const int REG_BANK_ADDR = 12;
	const int REG_ROW_ADDR = 13;
	const int REG_COL_ADDR = 14;
	const int REG_NUM_COLS = 11;

	int remaining_cycs = 0;

	// ===== BEGIN SoftMC Program =====

	program.add_inst(SMC_LI(target_bank, REG_BANK_ADDR));

	add_op_with_delay(program, SMC_PRE(REG_BANK_ADDR, 0, 1), 0, 0); // precharge all banks

	// program.add_inst(SMC_LI(NUM_ROWS, REG_NUM_ROWS));
	program.add_inst(SMC_LI(NUM_COLS_PER_ROW * 8, REG_NUM_COLS));

	program.add_inst(SMC_LI(8, CASR)); // Load 8 into CASR since each READ reads 8 columns
	program.add_inst(SMC_LI(1, BASR)); // Load 1 into BASR
	program.add_inst(SMC_LI(1, RASR)); // Load 1 into RASR

	/* ==== Read the data of rows in the row group ==== */

	for (auto &wr : wrs.rows) {
		program.add_inst(SMC_LI(wr.row_id, REG_ROW_ADDR));

		// activate the next row and increment the row address register
		add_op_with_delay(program, SMC_ACT(REG_BANK_ADDR, 0, REG_ROW_ADDR, 0), 0,
				  trcd_cycles - 1);

		// issue read cmds to read out the entire row and precharge
		program.add_inst(SMC_LI(0, REG_COL_ADDR));

		string new_lbl = createSMCLabel("READ_ROW");
		program.add_label(new_lbl);
		add_op_with_delay(program, SMC_READ(REG_BANK_ADDR, 0, REG_COL_ADDR, 1, 0, 0),
				  remaining_cycs, 4);
		remaining_cycs = 0;
		program.add_branch(program.BR_TYPE::BL, REG_COL_ADDR, REG_NUM_COLS, new_lbl);

		// Wait for t(write-precharge)
		// & precharge the open bank
		remaining_cycs =
			add_op_with_delay(program, SMC_PRE(REG_BANK_ADDR, 0, 0), 0, trp_cycles);
	}

	program.add_inst(SMC_END());
}

uint determineRowBatchSize(const uint retention_ms, const uint num_data_patterns)
{
	uint pcie_cycles = ceil(5000 / FPGA_PERIOD); // assuming 5us pcie transfer latency
	uint setup_cycles = 36;
	uint pattern_loop_cycles = 64 /*write reg init*/ + 1 /*ACT*/ + trcd_cycles + 4 +
				   (4 + 24) * NUM_COLS_PER_ROW /*row write*/ + 1 + trp_cycles;

	// cycles(retention_ms) = pcie_cycles + setup_cycles +
	// (X/NUM_PATTERNS)*(NUM_PATTERNS*pattern_loop_cycles + 28)
	//
	// X: batch size
	//
	// X = ((retention_cycles - pcie_cycles -
	// setup_cycles)/(NUM_PATTERNS*pattern_loop_cycles + 28))*NUM_PATTERNS
	ulong retention_cycles = floor((retention_ms * 1000000) / FPGA_PERIOD);

	uint batch_size = ((retention_cycles - pcie_cycles - setup_cycles) /
			   (num_data_patterns * pattern_loop_cycles + 28)) *
			  num_data_patterns;

	// cout << "Calculated initial batch size as " << batch_size << " for " << retention_ms << "
	// ms" << endl; cout << "Rounding batch_size to the previous power-of-two number" << endl;

	assert(NUM_ROWS % num_data_patterns == 0 &&
	       "Number of specified data patterns must be a divisor of NUM_ROWS, i.e., power of "
	       "two");

	// rounding
	batch_size = min(1 << (uint)(log2(batch_size)), NUM_ROWS);

	// cout << "The final batch_size: " << batch_size << endl;

	return batch_size;
}

// pairs of row_id and number of bitflips
static std::vector<std::pair<int, std::vector<uint> > > bitflip_history;
static std::vector<uint> locs_to_check;

vector<Row> getVector(const string &row_group_pattern);
// Initialize with row id -1 and empty bitflip vectors
void clear_bitflip_history(std::vector<std::pair<int, std::vector<uint> > > &history)
{
	for (auto &item : history) {
		item.first = -1;
		item.second.clear();
	}
}

void init_row_pattern_fitter(const std::string &row_group_pattern,
			     std::vector<std::pair<int, std::vector<uint> > > &history,
			     std::vector<uint> &locs_to_check)
{
	clear_bitflip_history(history);

	for (uint i = 0; i < row_group_pattern.size(); i++) {
		char c = row_group_pattern[i];
		if (c == 'R') {
			locs_to_check.push_back(i);
		}
	}
}

bool fitsIntoRowPattern(std::vector<std::pair<int, std::vector<uint> > > &bitflipHistory,
			std::vector<uint> &locsToCheck, const std::vector<uint> &bitflips,
			const uint rowId)
{
	// Check if receiving rowId in order
	if (!bitflipHistory.empty() && bitflipHistory.back().first != -1 &&
	    bitflipHistory.back().first != (rowId - 1)) {
		std::cerr << RED_TXT << "ERROR: Did not profile rows in order. Got row id " << rowId
			  << " after row id " << bitflipHistory.back().first << NORMAL_TXT
			  << std::endl;
		exit(-1);
	}

	// Remove the oldest entry in bitflipHistory
	if (!bitflipHistory.empty()) {
		bitflipHistory.erase(bitflipHistory.begin());
	}

	// Insert a new entry
	bitflipHistory.emplace_back(rowId, bitflips);

	// Check if the current bitflipHistory matches the pattern
	for (auto loc : locsToCheck) {
		if (bitflipHistory[loc].second.empty()) {
			return false;
		}
	}
	return true;
}

vector<Row> buildRowsFromHistory(const vector<std::pair<int, std::vector<uint> > > &bitflipHistory,
				 const vector<uint> &locsToCheck)
{
	vector<Row> rows;

	for (auto loc : locsToCheck) {
		auto &bfh = bitflipHistory[loc];
		auto row_id = to_logical_row_id(bfh.first);
		rows.emplace_back(row_id, bfh.second);
	}

	return rows;
}

void test_retention(SoftMCPlatform &platform, const uint retention_ms, const uint target_bank,
		    const uint first_row_id, const uint row_batch_size,
		    const vector<RowData> &rows_data, char *buf, vector<RowGroup> &rowGroups)
{
	// Write to DRAM
	Program writeProg;
	writeToDRAM(writeProg, target_bank, first_row_id, row_batch_size, rows_data);
	auto t_start_issue_prog = chrono::high_resolution_clock::now();
	platform.execute(writeProg);
	auto t_end_issue_prog = chrono::high_resolution_clock::now();

	// Wait for retention
	chrono::duration<double, milli> prog_issue_duration(t_end_issue_prog - t_start_issue_prog);
	waitMS(retention_ms - prog_issue_duration.count());

	// Read from DRAM
	auto t_prog_started = chrono::high_resolution_clock::now();
	Program readProg;
	readFromDRAM(readProg, target_bank, first_row_id, row_batch_size);
	platform.execute(readProg);
	platform.receiveData(buf, ROW_SIZE * row_batch_size); // reading all RH_NUM_ROWS at once

	// Check errors
	// go over physical row IDs in order
	for (int i = 0; i < row_batch_size; i++) {
		PhysicalRowID phys_row_id = first_row_id + i;
		LogicalRowID log_row_id = to_logical_row_id(phys_row_id);

		assert(log_row_id < (first_row_id + row_batch_size) && log_row_id >= first_row_id &&
		       "ERROR: The used Logical to Physical row address mapping results in logical "
		       "address out of bounds of the row_batch size. Consider revising the code.");

		char *readData = buf + (log_row_id - first_row_id) * ROW_SIZE;
		const auto& row = rows_data[(log_row_id - first_row_id) % rows_data.size()];
		auto bitflips = collect_bitflips(readData, row);

		if (fitsIntoRowPattern(bitflip_history, locs_to_check, bitflips, phys_row_id)) {
			// remove this if you are trying to enable support for different
			// input readData patterns for different rows
			assert(rows_data.size() == 1);

			auto dataPatternType = rows_data[0].pattern_id;
			auto rows = buildRowsFromHistory(bitflip_history, locs_to_check);

			RowGroup rowGroup = { rows, target_bank, retention_ms, dataPatternType, 0 };
			rowGroups.emplace_back(rowGroup);

			// to prevent a row being part of multiple row groups
			clear_bitflip_history(bitflip_history);
		}
	}
}

// return true if the same bit locations in RowGroup wrs experience bitflips
bool check_retention_failure_repeatability(SoftMCPlatform &platform, const uint retention_ms,
					   const uint target_bank, RowGroup &wrs,
					   const vector<RowData> &rows_data, char *buf,
					   bool filter_out_failures = false)
{
	Program writeProg;
	SoftMCRegAllocator reg_alloc(NUM_SOFTMC_REGS, reserved_regs);
	writeToDRAM(writeProg, reg_alloc, target_bank, wrs, rows_data);

	// execute the program
	auto t_start_issue_prog = chrono::high_resolution_clock::now();
	platform.execute(writeProg);
	auto t_end_issue_prog = chrono::high_resolution_clock::now();

	chrono::duration<double, milli> prog_issue_duration(t_end_issue_prog - t_start_issue_prog);

	waitMS(retention_ms - prog_issue_duration.count());

	// at this point we expect writing data to DRAM to be finished
	auto t_end_ret_wait = chrono::high_resolution_clock::now();

	chrono::duration<double, milli> write_ret_duration(t_end_ret_wait - t_start_issue_prog);

	// READ DATA BACK AND CHECK ERRORS
	auto t_prog_started = chrono::high_resolution_clock::now();
	Program readProg;
	readFromDRAM(readProg, target_bank, wrs);
	platform.execute(readProg);
	platform.receiveData(buf,
			     ROW_SIZE * wrs.rows.size()); // reading all RH_NUM_ROWS at once

	for (int i = 0; i < wrs.rows.size(); i++) {
		auto bitflips = collect_bitflips(buf + i * ROW_SIZE, rows_data[wrs.rowdata_ind]);

		// filter out bit locations that flipped
		if (filter_out_failures) {
			for (uint bf_loc : bitflips) {
				auto it = std::find(wrs.rows[i].bitflip_locs.begin(),
						    wrs.rows[i].bitflip_locs.end(), bf_loc);
				if (it != wrs.rows[i].bitflip_locs.end())
					wrs.rows[i].bitflip_locs.erase(it);
			}
		} else { // filter out bit locations that did not flip
			for (auto it = wrs.rows[i].bitflip_locs.begin();
			     it != wrs.rows[i].bitflip_locs.end(); it++) {
				auto it_find = std::find(bitflips.begin(), bitflips.end(), *it);
				if (it_find == bitflips.end())
					wrs.rows[i].bitflip_locs.erase(it--);
			}
		}

		// return false if all bitflip locations in a weak row were filtered out
		if (wrs.rows[i].bitflip_locs.empty())
			return false;
	}

	return true;
}

// check if the candicate row groups have repeatable retention bitflips according to the RETPROF
// configuration parameters clears candidate_weaks
void analyze_weaks(SoftMCPlatform &platform, const vector<RowData> &rows_data,
		   vector<RowGroup> &candidate_weaks, vector<RowGroup> &row_group,
		   const uint weak_rows_needed)
{
	for (auto &wr : candidate_weaks) {
		std::cout << BLUE_TXT << "Checking retention time consistency of row(s) "
			  << wr.toString() << NORMAL_TXT << std::endl;
		char buf[ROW_SIZE * wr.rows.size()];

		// Setting up a progress bar
		progresscpp::ProgressBar progress_bar(RETPROF_NUM_ITS, 70, '#', '-');
		progress_bar.display();

		bool success = true;
		for (uint i = 0; i < RETPROF_NUM_ITS; i++) {
			// test whether the row experiences bitflips with RETPROF_RETTIME_MULT_H
			// higher retention time
			if (!check_retention_failure_repeatability(
				    platform, (int)wr.ret_ms * RETPROF_RETTIME_MULT_H, wr.bank_id,
				    wr, rows_data, buf)) {
				progress_bar.done();
				std::cout << RED_TXT << "HIGH RETENTION CHECK FAILED" << NORMAL_TXT
					  << std::endl;
				success = false;
				break;
			}

			// test whether the row never experiences bitflips with
			// RETPROF_RETTIME_MULT_L lower retention time
			if (!check_retention_failure_repeatability(
				    platform, (int)wr.ret_ms * RETPROF_RETTIME_MULT_H * 0.5f,
				    wr.bank_id, wr, rows_data, buf, true)) {
				progress_bar.done();
				std::cout << RED_TXT << "LOW RETENTION CHECK FAILED" << NORMAL_TXT
					  << std::endl;
				success = false;
				break;
			}

			++progress_bar;
			progress_bar.display();
		}

		if (!success)
			continue;

		progress_bar.done();

		std::cout << MAGENTA_TXT << "PASSED" << NORMAL_TXT << std::endl;
		row_group.push_back(std::move(wr));

		if (row_group.size() == weak_rows_needed)
			break;
	}

	candidate_weaks.clear();

	// Sort the rows in each wrs based on the physical row IDs
	for (auto &wr : row_group) {
		std::sort(wr.rows.begin(), wr.rows.end(), [](const Row &lhs, const Row &rhs) {
			return to_physical_row_id(lhs.row_id) < to_physical_row_id(rhs.row_id);
		});
	}
}

std::string wrs_to_string(const RowGroup &wrs)
{
	return JS::serializeStruct(wrs);
}

int main(int argc, char **argv)
{
	string out_filename = "./out.txt";
	int target_bank = 1;
	int starting_ret_time = 64;
	int num_row_groups = 1;
	// to search for rows that have specific distances among each other.
	// For example, "R-R" (default) makes RowScout search for two rows that 1) are one row
	// address apart and 2) have similar retention times. Similarly, "RR" makes RowScout search
	// for two rows that 1) have consecutive row addresses and 2) have similar retention times.
	// "R" makes RowScout search for any row that would experience a retention failure.
	string row_group_pattern = "R-R";

	int input_data_pattern = 1;
	vector<int> row_range{ -1, -1 };

	uint arg_log_phys_conv_scheme = 0;

	bool append_output = false;

	// try{
	options_description desc("RowScout Options");
	desc.add_options()("help,h", "Prints this usage statement.")(
		"out,o", value(&out_filename)->default_value(out_filename),
		"Specifies a path for the output file.")(
		"bank,b", value(&target_bank)->default_value(target_bank),
		"Specifies the address of the bank to be profiled.")(
		"range", value<vector<int> >(&row_range)->multitoken(),
		"Specifies a range of row addresses (start and end values are both "
		"inclusive) to "
		"be profiled. By default, the range spans an entire bank.")(
		"init_ret_time,r", value(&starting_ret_time)->default_value(starting_ret_time),
		"Specifies the initial retention time (in milliseconds) to test the rows specified "
		"by --bank and --range. When RowScout cannot find a set of rows that satisfy the "
		"requirements specified by other options, RowScout increases the retention time "
		"used in profiling and repeats the profiling procedure.")(
		"row_group_pattern", value(&row_group_pattern)->default_value(row_group_pattern),
		"Specifies the distances among rows in a row group that RowScout must find. Must "
		"include only 'R' and '-'. Example values: R-R (two one-row-address-apart rows "
		"with similar retention times) , RR (two consecutively-addressed rows with similar "
		"retention times).")("num_row_groups,w",
				     value(&num_row_groups)->default_value(num_row_groups),
				     "Specifies the number of row groups that RowScout must find.")(
		"log_phys_scheme",
		value(&arg_log_phys_conv_scheme)->default_value(arg_log_phys_conv_scheme),
		"Specifies how to convert logical row IDs to physical row ids and the other way "
		"around. Pass 0 (default) for sequential mapping, 1 for the mapping scheme "
		"typically used in Samsung chips.")(
		"input_data,i", value(&input_data_pattern)->default_value(input_data_pattern),
		"Specifies the data pattern to initialize rows with for profiling. "
		"Defined value are 0: random, 1: all ones, 2: all zeros, 3: colstripe (0101), 4: "
		"inverse colstripe (1010), 5: checkered (0101, 1010), 6: inverse checkered (1010, "
		"0101)")("append", bool_switch(&append_output),
			 "When specified, the output is appended to the "
			 "--out file (if it exists). "
			 "Otherwise the --out file is cleared.");

	variables_map vm;
	store(parse_command_line(argc, argv, desc), vm);
	notify(vm);

	if (vm.count("help")) {
		cout << desc << endl;
		return 0;
	}

	assert(row_range.size() == 2 &&
	       "--row_range must specify exactly two tokens. E.g., <--row_range  0 5>");
	if (row_range[0] > row_range[1])
		swap(row_range[0], row_range[1]);

	if (row_range[0] < 0)
		row_range[0] = 0;

	if (row_range[1] < 0)
		row_range[1] = NUM_ROWS - 1;

	if (row_range[1] > (NUM_ROWS - 1)) {
		cout << "Specified row range exceeds the number of rows in the DRAM module. "
			"Adjusting the range accordingly."
		     << endl;
		cout << "Specified: " << row_range[1] << ", actual num rows: " << NUM_ROWS << endl;
		row_range[1] = NUM_ROWS - 1;
	}

	// make sure row_group_pattern contains only R(r) or -
	if (row_group_pattern.find_first_not_of("Rr-") != std::string::npos) {
		cerr << RED_TXT << "ERROR: --row_group_pattern should contain only R or -"
		     << NORMAL_TXT << std::endl;
		exit(-1);
	}

	// make sure row_group_pattern is uppercase
	for (auto &c : row_group_pattern)
		c = toupper(c);

	// the pattern should start with R and end with R
	row_group_pattern = row_group_pattern.substr(
		row_group_pattern.find_first_of('R'),
		row_group_pattern.find_last_of('R') - row_group_pattern.find_first_of('R') + 1);

	init_row_pattern_fitter(row_group_pattern, bitflip_history, locs_to_check);

	path out_dir(out_filename);
	out_dir = out_dir.parent_path();

	if (!(exists(out_dir))) {
		if (!create_directory(out_dir)) {
			cerr << "Cannot create directory: " << out_dir << ". Exiting..." << endl;
			return -1;
		}
	}

	boost::filesystem::ofstream out_file;
	if (append_output)
		out_file.open(out_filename, boost::filesystem::ofstream::app);
	else
		out_file.open(out_filename);

	SoftMCPlatform platform;
	int err;

	if ((err = platform.init()) != SOFTMC_SUCCESS) {
		cerr << "Could not initialize SoftMC Platform: " << err << endl;
		return err;
	}

	platform.reset_fpga();
	platform.set_aref(false); // disable refresh

	// init random data generator
	srand(0);

	assert(arg_log_phys_conv_scheme < uint(LogPhysRowIDScheme::MAX));
	logical_physical_conversion_scheme = (LogPhysRowIDScheme)arg_log_phys_conv_scheme;

	auto t_prog_started = chrono::high_resolution_clock::now();
	auto t_two_rows_recvd = chrono::high_resolution_clock::now();
	chrono::duration<double> elapsed{};

	const uint default_data_patterns[] = { 0x0,	   0xFFFFFFFF, 0x00000000, 0x55555555,
					       0xAAAAAAAA, 0xAAAAAAAA, 0x55555555 };

	// this is ugly but I am leaving it like this to make things easier in case we decide to use
	// different input data patterns for different rows.
	vector<RowData> rows_data;
	vector<int> input_data_patterns = { input_data_pattern };
	for (int inp_pat : input_data_patterns) {
		RowData rd;
		bitset<512> rdata;

		switch (inp_pat) {
		case 0: { // random
			// GENERATING RANDOM TEST DATA
			uint32_t rand_int;

			for (int pos = 0; pos < 16; pos++) {
				rdata <<= 32;
				rand_int = (rand() << 16) | (0x0000FFFF & rand());
				rdata |= rand_int;
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
				rdata <<= 32;
				rdata |= default_data_patterns[inp_pat];
			}

			break;
		}
		default: {
			cerr << "Undefined input data pattern mode: " << inp_pat << endl;
			return -1;
		}
		}

		rd.input_data_pattern = rdata;
		rd.pattern_id = inp_pat;
		rows_data.push_back(rd);
	}

	int retention_ms = starting_ret_time;
	uint64_t buf_size = 0;
	char *buf = nullptr;
	vector<RowGroup> candidateRowGroups;
	vector<RowGroup> rowGroups;

	uint num_wrs_written_out = 0;
	uint last_num_weak_rows = 0;

	while (true) {
		std::cout << "Profiling with " << retention_ms << " ms retention time" << std::endl;

		uint max_row_batch_size = determineRowBatchSize(retention_ms, rows_data.size());
		uint target_region_size = row_range[1] - row_range[0] + 1;
		uint row_batch_size = min(max_row_batch_size, target_region_size);

		// check the size of the buffer to read the data to and increase its size if needed
		if (buf_size < row_batch_size * ROW_SIZE) {
			delete[] buf;

			buf = new char[row_batch_size * ROW_SIZE];
			buf_size = row_batch_size * ROW_SIZE;
		}

		// apply the retention time to the corresponding row region
		uint num_profiled_rows = 0;
		while (num_profiled_rows < target_region_size) {
			clear_bitflip_history(bitflip_history);
			test_retention(platform, retention_ms, target_bank, row_range[0],
				       row_batch_size, rows_data, buf, candidateRowGroups);

			candidateRowGroups =
				filterCandidateRowGroups(rowGroups, candidateRowGroups);

			// analyze the candidate row groups to ensure the bitflips are repeatable
			// and the retention time is determined accurately (i.e., we do not want a
			// cell to fail for periods smaller than the determined retention time)
			if (!candidateRowGroups.empty()) {
				std::cout << RED_TXT << "Found " << candidateRowGroups.size()
					  << " new candidate row groups." << NORMAL_TXT
					  << std::endl;
				analyze_weaks(platform, rows_data, candidateRowGroups, rowGroups,
					      num_row_groups);
			}

			while (num_wrs_written_out < rowGroups.size()) {
				out_file << wrs_to_string(rowGroups[num_wrs_written_out++])
					 << std::endl;
			}

			if (rowGroups.size() >= num_row_groups)
				break;

			num_profiled_rows += row_batch_size;
		}

		auto cur_time = chrono::high_resolution_clock::now();
		elapsed = cur_time - t_prog_started;
		std::cout << GREEN_TXT << "[" << (int)elapsed.count() << " s] Found "
			  << rowGroups.size() - last_num_weak_rows << " new (" << rowGroups.size()
			  << " total) row groups" << NORMAL_TXT << std::endl;
		last_num_weak_rows = rowGroups.size();

		if (rowGroups.size() >= num_row_groups)
			break;

		retention_ms += (int)(starting_ret_time * RETPROF_RETTIME_STEP);
	}

	out_file.close();

	std::cout << "The test has finished!" << endl;

	return 0;
}
