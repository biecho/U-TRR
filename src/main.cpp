#include <boost/program_options.hpp>
#include <iostream>
#include <string>
#include <vector>

using namespace std;
using namespace boost::program_options;

// Function to setup command-line options
options_description setup_options()
{
	options_description desc("RowScout Options");
	desc.add_options()("help,h", "Prints this usage statement.")("out,o", value<string>()->default_value("default_output.txt"),
								     "Specifies a path for the output file.")(
		"bank,b", value<string>()->default_value(""), "Specifies the address of the bank to be profiled.")(
		"range", value<vector<int> >()->multitoken(), "Specifies a range of row addresses.")(
		"init_ret_time,r", value<int>()->default_value(100), "Specifies the initial retention time (in milliseconds).")(
		"row_group_pattern", value<string>()->default_value("RR"), "Specifies the distances among rows in a row group.")(
		"num_row_groups,w", value<int>()->default_value(1), "Specifies the number of row groups to find.")(
		"log_phys_scheme", value<int>()->default_value(0), "Specifies the logical to physical ID conversion scheme.")(
		"input_data,i", value<int>()->default_value(0), "Specifies the data pattern to initialize rows with for profiling.")(
		"append", bool_switch(), "When specified, the output is appended to the output file. Otherwise, the file is cleared.");
	return desc;
}

// Function to process command-line options
void process_options(const variables_map &vm)
{
	if (vm.count("help")) {
		cout << "Usage instructions\n";
		return;
	}

	cout << "Output filename: " << vm["out"].as<string>() << endl;
	cout << "Bank address: " << vm["bank"].as<string>() << endl;
	if (vm.count("range")) {
		cout << "Row range: ";
		for (int num : vm["range"].as<vector<int> >()) {
			cout << num << " ";
		}
		cout << endl;
	}
	cout << "Initial retention time: " << vm["init_ret_time"].as<int>() << " ms" << endl;
	cout << "Row group pattern: " << vm["row_group_pattern"].as<string>() << endl;
	cout << "Number of row groups: " << vm["num_row_groups"].as<int>() << endl;
	cout << "Logical to physical scheme: " << vm["log_phys_scheme"].as<int>() << endl;
	cout << "Input data pattern: " << vm["input_data"].as<int>() << endl;
	cout << "Append output: " << (vm["append"].as<bool>() ? "yes" : "no") << endl;
}

int main(int argc, char *argv[])
{
	try {
		options_description desc = setup_options();
		variables_map vm;
		store(parse_command_line(argc, argv, desc), vm);
		notify(vm);

		if (vm.count("help")) {
			cout << desc << endl;
			return 0;
		}

		process_options(vm);
	} catch (const error &ex) {
		cerr << "Error: " << ex.what() << endl;
		return 1;
	}

	return 0;
}
