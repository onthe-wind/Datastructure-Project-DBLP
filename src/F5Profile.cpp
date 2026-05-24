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
    os << "[性能画像] 执行路径=" << f5_exec_path_name(path)
       << " 结果缓存=" << (result_cache_hit ? "命中" : "未命中")
       << " 总耗时_ms=" << total_ms << " 解析_ms=" << parse_ms << " 改写_ms=" << rewrite_ms
       << " 打分_ms=" << score_ms << " 排序_ms=" << rank_ms << " 输出_ms=" << emit_ms << '\n';
    os << "[性能画像] 查询词 原始=" << raw_query_terms << " 打分=" << scoring_terms
       << " 命中=" << matched_query_terms << " 容错改写=" << fuzzy_rewrite_count
       << " 请求Top-K=" << requested_topk << '\n';
    os << "[性能画像] 倒排访问=" << postings_visited << " 倒排打分=" << postings_scored
       << " 剪枝块=" << blocks_pruned << " 触达文档=" << docs_touched
       << " 总命中=" << total_hits << " 已输出=" << results_emitted << '\n';
}
