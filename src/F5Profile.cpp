#include "F5Profile.h"

#include <iomanip>
#include <ostream>

const char* f5_exec_path_name(const F5ExecPath path) noexcept {
    switch (path) {
    case F5ExecPath::ResultCache:
        return "result_cache";
    case F5ExecPath::SingleTermBlockmax:
        return "single_term_blockmax";
    case F5ExecPath::OrBlockmax:
        return "or_blockmax";
    case F5ExecPath::DaatWandOr:
        return "daat_wand_or";
    case F5ExecPath::DaatWandAnd:
        return "daat_wand_and";
    case F5ExecPath::PhraseDaat:
        return "phrase_daat";
    case F5ExecPath::PrefixDaat:
        return "prefix_daat";
    case F5ExecPath::NewestYear:
        return "newest_year";
    case F5ExecPath::FullScan:
        return "full_scan";
    case F5ExecPath::NoHits:
        return "no_hits";
    case F5ExecPath::InvalidQuery:
        return "invalid_query";
    case F5ExecPath::Empty:
    default:
        return "empty";
    }
}

void F5SearchProfile::reset() noexcept {
    *this = F5SearchProfile{};
}

void F5SearchProfile::print(std::ostream& os) const {
    os << std::fixed << std::setprecision(3);
    os << "[Profile] path=" << f5_exec_path_name(path)
       << " cache=" << (result_cache_hit ? "hit" : "miss")
       << " total_ms=" << total_ms << " parse_ms=" << parse_ms << " rewrite_ms=" << rewrite_ms
       << " score_ms=" << score_ms << " rank_ms=" << rank_ms << " emit_ms=" << emit_ms << '\n';
    os << "[Profile] terms raw=" << raw_query_terms << " scoring=" << scoring_terms
       << " matched=" << matched_query_terms << " fuzzy_rewrite=" << fuzzy_rewrite_count
       << " requested_topk=" << requested_topk << '\n';
    os << "[Profile] postings visited=" << postings_visited << " scored=" << postings_scored
       << " blocks_pruned=" << blocks_pruned << " docs_touched=" << docs_touched
       << " total_hits=" << total_hits << " emitted=" << results_emitted << '\n';
}
