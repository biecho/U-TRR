#include "TRRAnalyzerConfig.h"
#include <iostream>
#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>

namespace po = boost::program_options;

TRRAnalyzerConfig parseCommandLine(int argc, char *argv[])
{
	TRRAnalyzerConfig config; // Configuration instance to hold all settings

	// Define and parse command-line options
	po::options_description desc("TRR Analyzer Options");
	desc.add_options()("help,h", "Prints this usage statement.")

		// Output arguments
		("out,o",
		 po::value<std::string>(&config.output.out_filename)->default_value("./out.txt"),
		 "Specifies a path for the output file.")(
			"append", po::bool_switch(&config.output.append_output),
			"When specified, the output of TRR Analyzer is appended to the --out file. "
			"Otherwise, the --out file is cleared.")(
			"location_out", po::bool_switch(&config.output.location_out),
			"When specified, the bit flip locations are written to the --out file.")

		// Row analysis
		("row_scout_file,f",
		 po::value<std::string>(&config.row_analysis.row_scout_file)->required(),
		 "A file containing a list of row groups and their retention times, i.e., the "
		 "output of RowScout.")(
			"num_row_groups,w",
			po::value<uint>(&config.row_analysis.num_row_groups)->default_value(1),
			"The number of row groups to work with. Row groups are parsed in order "
			"from the "
			"'row_scout_file'.")("row_layout",
					     po::value<std::string>(&config.row_analysis.row_layout)
						     ->default_value("RAR"),
					     "Specifies how the aggressor rows should be "
					     "positioned inside "
					     "a row group. Allowed characters are 'R', 'A', 'U', "
					     "and '-'. "
					     "For example, 'RAR' places an aggressor row between "
					     "two "
					     "adjacent (victim) rows, as is single-sided RowHammer "
					     "attacks. 'RARAR' places two aggressor rows to "
					     "perform "
					     "double-sided RowHammer attack. '-' specifies a row "
					     "that is "
					     "not to be hammered or checked for bit flips. 'U' "
					     "specifies a "
					     "(unified) row that will be both hammered and checked "
					     "for bit "
					     "flips.")(
			"row_group_indices",
			po::value<std::vector<uint> >(&config.row_analysis.row_group_indices)
				->multitoken(),
			"An optional argument used select which exact row groups in the "
			"--row_scout_file "
			"to use. When this argument is not provided, TRR Analyzer selects "
			"--num_row_groups "
			"from the file in order")

		// Experiment
		("num_rounds", po::value<uint>(&config.experiment.num_rounds)->default_value(0),
		 "Specifies the number of (hammer + refresh) rounds that the experiment "
		 "should "
		 "perform.")("num_iterations",
			     po::value<uint>(&config.experiment.num_iterations)->default_value(1),
			     "Defines how many times the sequence of {aggr/victim initialization, "
			     "hammer+ref rounds, reading back and checking for bit flips} should "
			     "be "
			     "performed.")(
			"init_to_hammerbw_delay",
			po::value<float>(&config.experiment.init_to_hammerbw_delay)
				->default_value(0.0f),
			"A float in range [0,1] that specifies the ratio of time to wait before "
			"performing --hammers_before_wait. The default value (0) means all the "
			"delay is "
			"inserted after performing --hammers_before_wait.")(
			"pre_init_nops",
			po::value<uint>(&config.experiment.pre_init_nops)->default_value(0),
			"Specifies the number of NOPs (as FPGA cycles, i.e., 4 DRAM cycles) to be "
			"inserted before victim/aggressor data initialization.")(
			"only_pick_rgs", po::bool_switch(&config.experiment.only_pick_rgs),
			"When specified, the test finds hammerable row groups rows in "
			"--row_scout_file, but it does not run the TRR analysis.")(
			"log_phys_scheme",
			po::value<uint>(&config.experiment.log_phys_scheme)->default_value(0),
			"Specifies how to convert logical row IDs to physical row ids and the "
			"other way around. Pass 0 (default) for sequential mapping, 1 for the "
			"mapping scheme typically used in Samsung chips.")(
			"use_single_softmc_prog",
			po::bool_switch(&config.experiment.use_single_softmc_prog),
			"When specified, the entire experiment executes as a single SoftMC "
			"program. This is to prevent SoftMC maintenance operations to kick in "
			"between multiple SoftMC programs. However, using this option may result "
			"in a very large program that may exceed the instruction limit.")

		// Bank config
		("num_bank0_hammers",
		 po::value<uint>(&config.bank_config.num_bank0_hammers)->default_value(0),
		 "Specifies how many times a row from bank 0 should be hammered after "
		 "hammering the aggressor and dummy rows.")(
			"num_pre_init_bank0_hammers",
			po::value<uint>(&config.bank_config.num_pre_init_bank0_hammers)
				->default_value(0),
			"Specifies how many times a row from bank 0 should be hammered before "
			"initializing data in victim and aggressor rows.")

		// Aggressor row related args
		// Hammer
		("hammers_per_round",
		 po::value<std::vector<uint> >(&config.hammer.hammers_per_round)->multitoken(),
		 "Specifies how many times each of the aggressors in --row_layout will be hammered "
		 "in a round. You must enter multiple values, one for each aggressor.")(
			"cascaded_hammer", po::bool_switch(&config.hammer.cascaded_hammer),
			"When specified, the aggressor and dummy rows are hammered in "
			"non-interleaved manner, i.e., one row is hammered "
			"--hammers_per_round times and then the next row is hammered. "
			"Otherwise, the aggressor and dummy rows get activated one after "
			"another --hammers_per_round times.")(
			"hammers_before_wait",
			po::value<std::vector<uint> >(&config.hammer.hammers_before_wait)
				->multitoken(),
			"Similar to --hammers_per_round but hammering happens right after data "
			"initialization before waiting for half of the retention time.")(
			"hammer_rgs_individually",
			po::bool_switch(&config.hammer.hammer_rgs_individually),
			"When specified, --hammers_per_round specifies hammers for each aggressor "
			"row for separately each row group. Otherwise, the same aggressor hammers "
			"are applied to all row groups.")(
			"skip_hammering_aggr", po::bool_switch(&config.hammer.skip_hammering_aggr),
			"When provided, the aggressor rows are not hammered but just used to pick "
			"locations for the dummy rows.")(
			"hammer_duration",
			po::value<uint>(&config.hammer.hammer_duration)->default_value(0),
			"Specifies the number of additional cycles to wait in row active state "
			"while hammering (tRAS + hammer_duration). The default is 0, i.e., tRAS)")(
			"hammer_cycle_time",
			po::value<float>(&config.hammer.hammer_cycle_time)->default_value(0.0f),
			"Specifies the time interval between two consecutive activations (the "
			"default and the minimum is tRAS + tRP).")(
			"init_aggrs_first", po::bool_switch(&config.hammer.init_aggrs_first),
			"When specified, the aggressor rows are initialized with a data pattern "
			"before the victim rows.")(
			"first_it_aggr_init_and_hammer",
			po::bool_switch(&config.hammer.first_it_aggr_init_and_hammer),
			"When specified, the aggressor rows are initialized and hammered only "
			"during the first iteration.")(
			"init_only_victims", po::bool_switch(&config.hammer.init_only_victims),
			"When specified, only the victim rows are initialized at the beginning of "
			"an iteration but not the aggressors.")

		// Refresh related args
		("refs_per_round",
		 po::value<uint>(&config.refresh.refs_per_round)->default_value(1),
		 "Specifies how many REF commands to issue at the end of a round, i.e., after "
		 "hammering.")("refs_after_init",
			       po::value<uint>(&config.refresh.refs_after_init)->default_value(0),
			       "Specifies the number of REF commands to issue right after "
			       "initializing data in DRAM rows.")(
			"pre_ref_delay",
			po::value<uint>(&config.refresh.pre_ref_delay)->default_value(0),
			"Specifies the number of cycles to wait before performing REFs specified "
			"by --refs_per_round. Must be 8 or larger if not 0 for this arg to take an "
			"effect.")

		// Dummy row related args
		("num_dummy_aggrs",
		 po::value<uint>(&config.dummy.num_dummy_aggressors)->default_value(0),
		 "Specifies the number of dummy aggressors to hammer in each round. The dummy row "
		 "addresses are selected such that they are different and in safe distance from "
		 "the actual aggressor rows.")(
			"dummy_aggrs_bank",
			po::value<int>(&config.dummy.dummy_aggrs_bank)->default_value(-1),
			"Specifies the bank address from which dummy rows should be selected. If "
			"not specified, TRR Analyzer picks dummy rows from the same bank as the "
			"row groups.")(
			"dummy_aggr_ids",
			po::value<std::vector<uint> >(&config.dummy.dummy_aggr_ids)->multitoken(),
			"Specifies the exact dummy row addresses to hammer in each round instead "
			"of letting TRR Analyzer select the dummy rows.")(
			"dummy_hammers_per_round",
			po::value<uint>(&config.dummy.dummy_hammers_per_round)->default_value(1),
			"Specifies how many times each dummy row is hammered in each round.")(
			"dummy_ids_offset",
			po::value<uint>(&config.dummy.dummy_ids_offset)->default_value(0),
			"Specifies a value to offset every dummy row address. Useful when there is "
			"a need to pick different dummy rows in different runs of TRR Analyzer.")(
			"hammer_dummies_first", po::bool_switch(&config.dummy.hammer_dummies_first),
			"When specified, the dummy rows are hammered before hammering the actual "
			"aggressor rows.")(
			"hammer_dummies_independently",
			po::bool_switch(&config.dummy.hammer_dummies_independently),
			"When specified, the dummy rows are hammered after the aggressor rows "
			"regardless of whether --cascaded is used or not. The dummy rows are "
			"simply treated as a separate group of rows to hammer after hammering the "
			"aggressor rows in interleaved or cascaded way.")(
			"num_dummy_after_init",
			po::value<uint>(&config.dummy.num_dummy_after_init)->default_value(0),
			"Specifies the number of dummy rows to hammer right after initializing the "
			"victim and aggressor rows. These dummy row hammers happen concurrently "
			"with --refs_after_init refreshes. Each dummy is hammered as much as "
			"possible based on the refresh interval and --refs_after_init.")(
			"refs_after_init_no_dummy_hammer",
			po::bool_switch(&config.dummy.refs_after_init_no_dummy_hammer),
			"When specified, after hammering dummy rows as specified by "
			"--num_dummy_after_init, TRR Analyzer also performs another set of "
			"refreshes but this time without hammering dummy rows.")(
			"first_it_dummy_hammer",
			po::bool_switch(&config.dummy.first_it_dummy_hammer),
			"When specified, the dummy rows are hammered only during the first "
			"iteration.");

	// Parsing command line into variable map
	po::variables_map vm;
	try {
		po::store(po::parse_command_line(argc, argv, desc), vm);

		if (vm.count("help")) {
			std::cout << desc << "\n";
			exit(0); // Or handle --help option appropriately
		}

		po::notify(vm); // This will throw if any of the required options are not provided.
	} catch (const po::error &ex) {
		std::cerr << ex.what() << '\n';
		std::cerr << desc << std::endl;
		exit(1);
	}

	return config;
}

void ensureDirectoryExists(const std::string &filePath)
{
	boost::filesystem::path path(filePath);
	boost::filesystem::path directory = path.parent_path();

	if (!directory.empty() && !boost::filesystem::exists(directory)) {
		if (!boost::filesystem::create_directories(directory)) {
			throw std::runtime_error("Failed to create directory: " +
						 directory.string());
		}
	}
}

std::ofstream openFile(const std::string &filePath, bool append)
{
	std::ofstream fileStream;
	if (!filePath.empty()) {
		std::ios_base::openmode mode = std::ios_base::out |
					       (append ? std::ios_base::app : std::ios_base::trunc);
		fileStream.open(filePath, mode);
		if (!fileStream.is_open()) {
			throw std::runtime_error("Failed to open file: " + filePath);
		}
	} else {
		fileStream.open("/dev/null");
		if (!fileStream.is_open()) {
			throw std::runtime_error("Failed to open /dev/null");
		}
	}
	return fileStream;
}
