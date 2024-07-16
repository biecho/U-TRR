#pragma once

#include <cmath> // for std::ceil
#include "prog.h"
#include "RowGroup.h"

#define CASR 0
#define BASR 1
#define RASR 2

#define NUM_SOFTMC_REGS 16

extern float FPGA_PERIOD; // ns

/*** DRAM Organization Parameters ***/
extern int NUM_BANKS;
extern int NUM_BANK_GROUPS;
extern int NUM_ROWS;
extern int ROW_SIZE_BYTES;
extern int NUM_COLS_PER_ROW;

/*** DRAM Timing Parameters ***/
extern float DEFAULT_TRCD;
extern float DEFAULT_TRAS;
extern float DEFAULT_TRP;
extern float DEFAULT_TWR;
extern float DEFAULT_TRFC;
extern float DEFAULT_TRRDS;
extern float DEFAULT_TRRDL;
extern float DEFAULT_TREFI;

extern int trcd_cycles;
extern int tras_cycles;
extern int trp_cycles;
extern int twr_cycles;
extern int trfc_cycles;
extern int trrds_cycles;
extern int trrdl_cycles;
extern int trefi_cycles;

void writeToDRAM(Program &program, uint target_bank, uint start_row,
		 uint row_batch_size, const vector<RowData> &rows_data);

void readFromDRAM(Program &program, uint target_bank, uint start_row, uint row_batch_size);

uint determineRowBatchSize(uint retention_ms, uint num_data_patterns);
