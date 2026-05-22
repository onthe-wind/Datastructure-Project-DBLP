#pragma once

#include "F5Profile.h"

#include <iosfwd>
#include <string>
#include <vector>

class ExtremeEngine;

struct QuerySuiteEntry {
    std::string id;
    std::string category;
    std::string query;
};

[[nodiscard]] std::vector<QuerySuiteEntry> load_query_suite(const char* file_name = "query_suite.txt");

struct QueryBenchmarkRow {
    std::string id;
    std::string category;
    std::string query;
    F5SearchProfile profile;
    bool ok = false;
};

void run_query_benchmark(const ExtremeEngine& engine, std::ostream& os, const char* suite_path = "query_suite.txt");
