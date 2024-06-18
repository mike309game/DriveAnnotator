#include <iostream>
#include <vector>
#include <algorithm>
#include <string>
#include <fstream>
#include <assert.h>

int main(int argc, char** argv) {
	setlocale(LC_ALL, "en_US.UTF-8");
	std::string accum;
	
	std::ifstream in("WizTree_20240604133928.csv");
	std::ofstream out("wiztreefolders.txt");
	std::vector<std::string> sort;
	for (;;) {
		std::getline(in, accum);
		if (accum == "") break;
		auto argspos = accum.find_first_of('"', 1);
		auto fname = accum.substr(1, argspos-1);
		if (*(fname.end()-1) == '\\') {
			fname = fname.substr(1, fname.length()-2);
			fname[0] = '.';
			sort.push_back(fname);
		}
		if (in.eof()) break;
	}
	std::sort(sort.begin(), sort.end());
	for (auto& i : sort) {
		out << i << std::endl;
	}
	out.close();
	return 0;
}