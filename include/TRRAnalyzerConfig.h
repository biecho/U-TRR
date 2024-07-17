#pragma once

#include <vector>
#include <string>
#include <boost/program_options.hpp>

// Configuration for row analysis
struct RowAnalysisConfig {
	std::string row_scout_file;
	std::vector<uint> row_group_indices;
	uint num_row_groups = 1;
	std::string row_layout = "RAR";
};

// Configuration for the hammering process
struct HammerConfig {
	std::vector<uint> hammers_per_round;
	std::vector<uint> hammers_before_wait;
	bool cascaded_hammer = false;
	bool hammer_rgs_individually = false;
	bool skip_hammering_aggr = false;
	uint hammer_duration = 0;
	float hammer_cycle_time = 0.0f;
	bool init_aggrs_first = false;
	bool first_it_aggr_init_and_hammer = false;
	bool init_only_victims = false;
};

// Configuration for dummy rows used in hammering
struct DummyRowConfig {
	uint num_dummy_aggressors = 0;
	int dummy_aggrs_bank = -1;
	std::vector<uint> dummy_aggr_ids;
	uint dummy_hammers_per_round = 1;
	uint dummy_ids_offset = 0;
	bool hammer_dummies_first = false;
	bool hammer_dummies_independently = false;
	uint num_dummy_after_init = 0;
	bool refs_after_init_no_dummy_hammer;
	bool first_it_dummy_hammer;
};

// Configuration for refresh commands in the experiment
struct RefreshConfig {
	uint refs_per_round = 1;
	uint refs_after_init = 0;
	bool refs_after_init_no_dummy_hammer = false;
	uint pre_ref_delay = 0; // Integrated into refresh-related configurations
};


// Configuration for the experimental process
struct ExperimentConfig {
	uint num_rounds = 0;
	uint num_iterations = 1;
	float init_to_hammerbw_delay = 0.0f;
	uint pre_init_nops = 0; // Moved here for setup-related configurations
	bool only_pick_rgs = false;
	uint log_phys_scheme = 0; // For addressing configurations
	bool use_single_softmc_prog = false;
};

// Configuration for output management
struct OutputConfig {
	std::string out_filename = "./out.txt";
	bool append_output = false;
	bool location_out = false;
};

struct MemoryBankConfig {
	uint num_bank0_hammers = 0;
	uint num_pre_init_bank0_hammers = 0;
};

// Main configuration structure comprising all sub-configurations
struct TRRAnalyzerConfig {
	RowAnalysisConfig row_analysis;
	HammerConfig hammer;
	DummyRowConfig dummy;
	RefreshConfig refresh;
	ExperimentConfig experiment;
	OutputConfig output;
	MemoryBankConfig bank_config;
};

// Function prototype to parse command line arguments into configuration
TRRAnalyzerConfig parseCommandLine(int argc, char *argv[]);

std::ofstream openFile(const std::string& filePath, bool append);
void ensureDirectoryExists(const std::string& filePath);
