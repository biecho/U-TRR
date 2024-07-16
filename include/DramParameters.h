#pragma once

#include <cmath> // for std::ceil

#define FPGA_PERIOD 1.5015f // ns

/*** DRAM Organization Parameters - UPDATE here if the organization of your DRAM module differs ***/
int NUM_BANKS = 16; // this is the total number of banks in the chip
int NUM_BANK_GROUPS = 4;
int NUM_ROWS = 32768;
int ROW_SIZE_BYTES = 8192;
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
float DEFAULT_TREFI = 7800.0f;
/******/

int trcd_cycles = (int)ceil(DEFAULT_TRCD / FPGA_PERIOD);
int tras_cycles = (int)ceil(DEFAULT_TRAS / FPGA_PERIOD);
int trp_cycles = (int)ceil(DEFAULT_TRP / FPGA_PERIOD);
int twr_cycles = (int)ceil(DEFAULT_TWR / FPGA_PERIOD);
int trfc_cycles = (int)ceil(DEFAULT_TRFC / FPGA_PERIOD);
int trrds_cycles = (int)ceil(DEFAULT_TRRDS / FPGA_PERIOD);
int trrdl_cycles = (int)ceil(DEFAULT_TRRDL / FPGA_PERIOD);
int trefi_cycles = (int)ceil(DEFAULT_TREFI / FPGA_PERIOD);

