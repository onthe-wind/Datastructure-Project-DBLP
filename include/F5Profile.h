#pragma once

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>

enum class F5ExecPath : std::uint8_t {
    Empty = 0,
    ResultCache = 1,
    SingleTermBlockmax = 2,
    OrBlockmax = 3,
    FullScan = 4,
    NoHits = 5,
    InvalidQuery = 6,
    DaatWandOr = 7,
    DaatWandAnd = 8,
    PhraseDaat = 9,
    PrefixDaat = 10,
    NewestYear = 11,
};

[[nodiscard]] const char* f5_exec_path_name(F5ExecPath path) noexcept;

struct F5SearchProfile {
    double parse_ms = 0.0;
    double rewrite_ms = 0.0;
    double score_ms = 0.0;
    double rank_ms = 0.0;
    double emit_ms = 0.0;
    double total_ms = 0.0;

    F5ExecPath path = F5ExecPath::Empty;
    bool result_cache_hit = false;

    std::size_t raw_query_terms = 0;
    std::size_t scoring_terms = 0;
    std::size_t matched_query_terms = 0;
    std::size_t fuzzy_rewrite_count = 0;
    std::size_t postings_visited = 0;
    std::size_t postings_scored = 0;
    std::size_t blocks_pruned = 0;
    std::size_t docs_touched = 0;
    std::size_t total_hits = 0;
    std::size_t results_emitted = 0;
    std::size_t requested_topk = 0;

    void reset() noexcept;
    void print(std::ostream& os) const;
};

struct F5SearchOptions {
    bool emit_results = true;
    F5SearchProfile* profile = nullptr;
};
