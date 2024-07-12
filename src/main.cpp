#include <algorithm>
#include <array>
#include <bitset>
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <list>
#include <numeric>
#include <regex>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <unistd.h>
#include <vector>

#include <string>
#include <vector>

#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>
using namespace boost::program_options;
using namespace boost::filesystem;

using namespace std;

int main(int argc, char **argv)
{
	options_description desc("RowScout Options");
	desc.add_options()("help,h", "Prints this usage statement.")(
		"out,o", value(&out_filename)->default_value(out_filename),
		"Specifies a path for the output file.")(
		"bank,b", value(&target_bank)->default_value(target_bank),
		"Specifies the address of the bank to be profiled.")(
		"range", value<vector<int> >(&row_range)->multitoken(),
		"Specifies a range of row addresses (start and end values are both inclusive) to "
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
		"Specifies the data pattern to initialize rows with for profiling. Defined value "
		"are 0: random, 1: all ones, 2: all zeros, 3: colstripe (0101), 4: inverse "
		"colstripe (1010), 5: checkered (0101, 1010), 6: inverse checkered (1010, 0101)")(
		"append", bool_switch(&append_output),
		"When specified, the output is appended to the --out file (if it exists). "
		"Otherwise the --out file is cleared.");

	variables_map vm;
	store(parse_command_line(argc, argv, desc), vm);
	notify(vm);

	string out_filename = "./out.txt";
	int test_mode = 0;
	int target_bank = 1;
	int target_row = -1;
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


    printf("Hello World\n");

    return 0;
}
