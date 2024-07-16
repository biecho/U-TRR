#include "Dram.h"
#include "softmc_utils.h"

float FPGA_PERIOD = 1.5015f;

int NUM_BANKS = 16; // this is the total number of banks in the chip
int NUM_BANK_GROUPS = 4;
int NUM_ROWS = 32768;
int ROW_SIZE_BYTES = 8192;
int NUM_COLS_PER_ROW = 128;

float DEFAULT_TRCD = 13.5f; // ns
float DEFAULT_TRAS = 35.0f; // ns
float DEFAULT_TRP = 13.5f; // ns
float DEFAULT_TWR = 15.0f; // ns
float DEFAULT_TRFC = 260.0f; // ns
float DEFAULT_TRRDS = 5.3f; // ns (ACT-ACT to different bank groups)
float DEFAULT_TRRDL = 6.4f; // ns (ACT-ACT to same bank group)
float DEFAULT_TREFI = 7800.0f;

int trcd_cycles = (int)ceil(DEFAULT_TRCD / FPGA_PERIOD);
int tras_cycles = (int)ceil(DEFAULT_TRAS / FPGA_PERIOD);
int trp_cycles = (int)ceil(DEFAULT_TRP / FPGA_PERIOD);
int twr_cycles = (int)ceil(DEFAULT_TWR / FPGA_PERIOD);
int trfc_cycles = (int)ceil(DEFAULT_TRFC / FPGA_PERIOD);
int trrds_cycles = (int)ceil(DEFAULT_TRRDS / FPGA_PERIOD);
int trrdl_cycles = (int)ceil(DEFAULT_TRRDL / FPGA_PERIOD);
int trefi_cycles = (int)ceil(DEFAULT_TREFI / FPGA_PERIOD);

void writeToDRAM(Program &program, uint target_bank, uint start_row, uint row_batch_size,
		 const vector<RowData> &rows_data)
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
		program.add_branch(Program::BL, REG_COL_ADDR, REG_NUM_COLS, new_lbl);

		// Wait for t(write-precharge)
		// & precharge the open bank
		remaining_cycs =
			add_op_with_delay(program, SMC_PRE(REG_BANK_ADDR, 0, 0), 0, trp_cycles);
	}

	program.add_inst(SMC_ADDI(REG_BATCH_IT, rows_data.size(), REG_BATCH_IT));
	program.add_branch(Program::BL, REG_BATCH_IT, REG_BATCH_SIZE, "INIT_BATCH");

	program.add_inst(SMC_END());
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
