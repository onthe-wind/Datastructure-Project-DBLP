#include "QuerySuite.h"

#include "ExtremeEngine.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

namespace {

[[nodiscard]] std::string trim_copy(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) {
        s.remove_suffix(1);
    }
    return std::string(s);
}

[[nodiscard]] std::filesystem::path locate_suite_file(const char* file_name) {
    namespace fs = std::filesystem;
    const fs::path file_path{file_name};
    const std::array<fs::path, 8> candidates = {
        fs::path("config") / file_path,
        fs::path("..") / "config" / file_path,
        fs::path("..") / ".." / "config" / file_path,
        fs::path("..") / ".." / ".." / "config" / file_path,
        fs::path("..") / file_path,
        fs::path("..") / ".." / file_path,
        fs::path("..") / ".." / ".." / file_path,
        file_path,
    };
    for (const fs::path& p : candidates) {
        std::error_code ec;
        if (fs::exists(p, ec) && !ec) {
            return p;
        }
    }
    return {};
}

[[nodiscard]] bool parse_suite_line(std::string_view line, QuerySuiteEntry& out) {
    line = trim_copy(line);
    if (line.empty() || line.front() == '#') {
        return false;
    }

    const std::size_t p1 = line.find('|');
    if (p1 == std::string_view::npos) {
        return false;
    }
    const std::size_t p2 = line.find('|', p1 + 1);
    if (p2 == std::string_view::npos) {
        return false;
    }

    out.id = trim_copy(line.substr(0, p1));
    out.category = trim_copy(line.substr(p1 + 1, p2 - p1 - 1));
    out.query = trim_copy(line.substr(p2 + 1));
    return !out.id.empty() && !out.query.empty();
}

[[nodiscard]] double percentile(std::vector<double>& values, double p) {
    if (values.empty()) {
        return 0.0;
    }
    if (p <= 0.0) {
        return values.front();
    }
    if (p >= 1.0) {
        return values.back();
    }
    const double rank = p * static_cast<double>(values.size() - 1);
    const std::size_t lo = static_cast<std::size_t>(rank);
    const std::size_t hi = std::min(values.size() - 1, lo + 1);
    const double frac = rank - static_cast<double>(lo);
    return values[lo] * (1.0 - frac) + values[hi] * frac;
}

} // namespace

std::vector<QuerySuiteEntry> load_query_suite(const char* file_name) {
    std::vector<QuerySuiteEntry> entries;
    const std::filesystem::path path = locate_suite_file(file_name);
    if (path.empty()) {
        return entries;
    }

    std::ifstream in(path);
    if (!in.is_open()) {
        return entries;
    }

    std::string raw;
    while (std::getline(in, raw)) {
        QuerySuiteEntry entry;
        if (parse_suite_line(raw, entry)) {
            entries.push_back(std::move(entry));
        }
    }
    return entries;
}

void run_query_benchmark(const ExtremeEngine& engine, std::ostream& os, const char* suite_path) {
    const std::vector<QuerySuiteEntry> suite = load_query_suite(suite_path);
    if (suite.empty()) {
        os << "错误: 未找到或无法解析查询评测集 \"" << suite_path << "\"。\n";
        os << "请将文件放在 config/query_suite.txt。\n";
        return;
    }

    os << "\n========== F5 Query Benchmark ==========\n";
    os << "[Suite] " << suite.size() << " queries from " << suite_path << "\n";
    os << "[Corpus] docs=" << engine.document_count() << " avgdl=" << engine.average_doc_length() << "\n\n";

    std::vector<QueryBenchmarkRow> rows;
    rows.reserve(suite.size());

    for (const QuerySuiteEntry& q : suite) {
        QueryBenchmarkRow row;
        row.id = q.id;
        row.category = q.category;
        row.query = q.query;
        row.profile.reset();

        std::ostringstream sink;
        F5SearchOptions opts;
        opts.emit_results = false;
        opts.profile = &row.profile;

        const auto t0 = std::chrono::steady_clock::now();
        engine.search_bm25(q.query, sink, opts);
        const auto t1 = std::chrono::steady_clock::now();
        if (row.profile.total_ms <= 0.0) {
            row.profile.total_ms =
                std::chrono::duration<double, std::milli>(t1 - t0).count();
        }
        row.ok = row.profile.path != F5ExecPath::InvalidQuery && row.profile.path != F5ExecPath::Empty;
        rows.push_back(std::move(row));
    }

    os << std::left << std::setw(6) << "id" << std::setw(14) << "category" << std::setw(12) << "path"
       << std::setw(10) << "total_ms" << std::setw(8) << "hits" << std::setw(10) << "postings"
       << std::setw(8) << "docs" << "query\n";
    os << std::string(96, '-') << '\n';

    for (const QueryBenchmarkRow& row : rows) {
        os << std::left << std::setw(6) << row.id << std::setw(14) << row.category << std::setw(12)
           << f5_exec_path_name(row.profile.path) << std::setw(10) << std::fixed << std::setprecision(2)
           << row.profile.total_ms << std::setw(8) << row.profile.total_hits << std::setw(10)
           << row.profile.postings_visited << std::setw(8) << row.profile.docs_touched << row.query << '\n';
    }

    std::vector<double> latencies;
    latencies.reserve(rows.size());
    for (const QueryBenchmarkRow& row : rows) {
        latencies.push_back(row.profile.total_ms);
    }
    std::sort(latencies.begin(), latencies.end());

    double sum = 0.0;
    std::size_t cache_hits = 0;
    std::size_t full_scan = 0;
    std::uint64_t postings_sum = 0;
    for (const QueryBenchmarkRow& row : rows) {
        sum += row.profile.total_ms;
        if (row.profile.result_cache_hit) {
            ++cache_hits;
        }
        if (row.profile.path == F5ExecPath::FullScan) {
            ++full_scan;
        }
        postings_sum += row.profile.postings_visited;
    }

    os << "\n========== Summary ==========\n";
    os << "[Latency] min=" << (latencies.empty() ? 0.0 : latencies.front())
       << " ms p50=" << percentile(latencies, 0.50) << " ms p95=" << percentile(latencies, 0.95)
       << " ms p99=" << percentile(latencies, 0.99)
       << " ms max=" << (latencies.empty() ? 0.0 : latencies.back())
       << " ms mean=" << (rows.empty() ? 0.0 : sum / static_cast<double>(rows.size())) << " ms\n";
    os << "[Paths] result_cache=" << cache_hits << " full_scan=" << full_scan << '\n';
    os << "[Load] postings_visited_sum=" << postings_sum << '\n';

    os << "\n========== Per-query profiles (last run) ==========\n";
    for (const QueryBenchmarkRow& row : rows) {
        os << "\n--- " << row.id << " (" << row.category << ") ---\n";
        row.profile.print(os);
    }
}
