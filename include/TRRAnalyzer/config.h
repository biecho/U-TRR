#pragma once

#include <vector>
#include <string>
#include <boost/program_options.hpp>

// Namespace to manage CLI configurations for the TRR Analyzer
namespace CLIConfig {

// Configuration for row analysis
struct RowAnalysisConfig {
	// Path to a file containing row groups and their retention times
	std::string row_scout_file = "";
	// Indices of specific row groups to use from the row scout file
	std::vector<uint> row_group_indices;
	// Number of row groups to work with from the row scout file
	uint num_row_groups = 1;
	// Defines the positioning of aggressor and victim rows within a row group
	std::string row_layout = "RAR";
};

// Configuration for the hammering process
struct HammerConfig {
	// Number of times each aggressor in the row layout is hammered per round
	std::vector<uint> hammers_per_round;
	// Number of hammers immediately after data initialization, before retention time wait
	std::vector<uint> hammers_before_wait;
	// If true, rows are hammered one by one instead of in an interleaved fashion
	bool cascaded_hammer = false;
	// If true, hammers per round are applied individually for each row group
	bool hammer_rgs_individually = false;
	// If true, skip hammering aggressor rows
	bool skip_hammering_aggr = false;
	// Additional cycles to wait in row active state while hammering (as DDR cycles, 1.5ns)
	uint hammer_duration = 0;
	// Time interval between two consecutive activations (as nanoseconds)
	float hammer_cycle_time = 0.0f;
	// If true, initialize aggressor rows with a data pattern before victim rows
	bool init_aggrs_first = false;
	// If true, initialize and hammer aggressor rows only during the first iteration
	bool first_it_aggr_init_and_hammer = false;
	// If true, only victim rows are initialized at the start of an iteration
	bool init_only_victims = false;
};

// Configuration for dummy rows used in hammering
struct DummyRowConfig {
	// Number of dummy aggressors to hammer in each round
	uint num_dummy_aggressors = 0;
	// Bank address from which dummy rows are selected (-1 means not specified)
	int dummy_aggrs_bank = -1;
	// Specific dummy row addresses to hammer in each round
	std::vector<uint> dummy_aggr_ids;
	// Number of times each dummy row is hammered per round
	uint dummy_hammers_per_round = 1;
	// Offset value for each dummy row address
	uint dummy_ids_offset = 0;
	// If true, dummy rows are hammered before the actual aggressor rows
	bool hammer_dummies_first = false;
	// If true, dummy rows are hammered independently of the aggressor rows
	bool hammer_dummies_independently = false;
};

// Configuration for refresh commands in the experiment
struct RefreshConfig {
	// Number of REF commands issued at the end of a round after hammering
	uint refs_per_round = 1;
	// Number of REF commands issued immediately after initializing data in DRAM rows
	uint refs_after_init = 0;
	// If true, perform REFs after dummy hammering without additional dummy row hammers
	bool refs_after_init_no_dummy_hammer = false;
};

// Configuration for the experimental process
struct ExperimentConfig {
	// Number of (hammer + refresh) rounds in the experiment
	uint num_rounds = 0;
	// Number of iterations of the hammer and check sequence
	uint num_iterations = 1;
	// Ratio of time to wait before performing hammers before wait (0 means no delay)
	float init_to_hammerbw_delay = 0.0f;
};

// Configuration for output management
struct OutputConfig {
	// Path for the output file
	std::string out_filename = "./out.txt";
	// If true, append output to the file; otherwise, overwrite
	bool append_output = false;
	// If true, write the bit flip locations to the output file
	bool location_out = false;
};

// Main configuration structure comprising all sub-configurations
struct MainConfig {
	RowAnalysisConfig row_analysis;
	HammerConfig hammer;
	DummyRowConfig dummy_row;
	RefreshConfig refresh;
	ExperimentConfig experiment;
	OutputConfig output;
};

// Function prototype to parse command line arguments into configuration
MainConfig parseCommandLine(int argc, char* argv[]);

} // namespace CLIConfig
