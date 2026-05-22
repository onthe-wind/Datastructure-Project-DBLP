#pragma once

#include "Document.h"
#include "ExtremeParser.h"
#include "F5Profile.h"
#include "Infrastructure.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

class ExtremeEngine {
public:
    ExtremeEngine() = default;

    void merge_local_indexes(std::vector<std::unique_ptr<LocalIndex>> locals);
    [[nodiscard]] bool try_load_serving_index();
    bool save_serving_index() const;

    void search_by_author(std::string_view query, std::ostream& os) const;
    void search_by_title(std::string_view query, std::ostream& os) const;

    void search_collaborators(std::string_view target_author, std::ostream& os) const;

    void search_bm25(std::string_view keywords, std::ostream& os,
                     F5SearchOptions opts = F5SearchOptions{}) const;

    void execute_f3_author_stats() const;
    void execute_f4_conference_analytics() const;
    void execute_f6_global_ranking() const;
    void execute_f7_export_report() const;

    [[nodiscard]] std::size_t document_count() const noexcept { return forward_index_.size(); }
    [[nodiscard]] float average_doc_length() const noexcept { return avg_dl_; }

    void print_document_details(const Document& doc, std::ostream& os) const;

private:
    struct F5PostingBlockMeta {
        DocID end_doc = 0;
        std::uint16_t max_tf = 0;
    };

    struct F5DaatTermState {
        const std::vector<Posting>* postings = nullptr;
        std::size_t pos = 0;
        float idf = 0.0f;
        float max_score = 0.0f;
    };

    struct F5ResultCacheEntry {
        std::vector<std::pair<float, DocID>> ordered_prefix;
        std::size_t total_hits = 0;
        std::size_t matched_query_terms = 0;
        std::size_t fuzzy_rewrite_count = 0;
    };

    enum class F5MatchKind : std::uint8_t { Exact = 0, Prefix = 1, Substring = 2 };

    struct F5QueryClause {
        F5MatchKind match = F5MatchKind::Exact;
        std::string_view token;
    };

    struct F4HotEntry {
        std::string term;
        int freq = 0;
        int prev_freq = 0;
        double burst_score = 0.0;
        double trend_score = 0.0;
    };

    [[nodiscard]] std::string_view build_normalized_lookup_key(std::string_view raw_query) const;

    void rebuild_author_and_title_inverted_from_forward();
    void rebuild_f1_author_fuzzy_index();
    void rebuild_f5_partial_match_index();
    [[nodiscard]] bool try_load_f5_keyword_segment(std::size_t expected_doc_count);
    void save_f5_keyword_segment(std::size_t doc_count) const;
    void rebuild_f3_top100_cache();
    void rebuild_year_doc_index();
    void rebuild_f4_year_term_cache();
    void collect_f1_fuzzy_author_candidates(
        std::string_view typo, int max_edits, std::vector<std::pair<std::string_view, float>>& out_authors) const;
    void collect_f5_prefix_candidates(std::string_view prefix, std::vector<std::string_view>& out_terms) const;
    void collect_f5_substring_candidates(std::string_view needle, std::vector<std::string_view>& out_terms) const;
    void collect_f5_fuzzy_candidates(
        std::string_view typo, int max_edits, std::vector<std::pair<std::string_view, float>>& out_terms) const;
    [[nodiscard]] bool try_search_f5_single_term_blockmax(
        std::string_view term, float query_boost, std::size_t requested_topk,
        std::vector<std::pair<float, DocID>>& ordered, std::size_t& total_hits,
        F5SearchProfile* profile) const;
    [[nodiscard]] bool try_search_f5_or_blockmax(
        const std::vector<std::pair<std::string_view, float>>& terms, std::size_t requested_topk,
        std::vector<std::pair<float, DocID>>& ordered, std::size_t& total_hits,
        F5SearchProfile* profile) const;
    [[nodiscard]] bool try_search_f5_daat_wand(
        const std::vector<std::pair<std::string_view, float>>& terms, bool require_all_terms,
        std::size_t requested_topk, std::vector<std::pair<float, DocID>>& ordered, std::size_t& total_hits,
        F5SearchProfile* profile) const;
    [[nodiscard]] bool try_search_f5_newest_year(
        const std::vector<std::pair<std::string_view, float>>& terms, bool require_all_terms,
        std::size_t requested_topk, std::vector<std::pair<float, DocID>>& ordered, std::size_t& total_hits,
        F5SearchProfile* profile) const;
    [[nodiscard]] bool try_search_f5_prefix_topk(
        const std::vector<std::pair<std::string_view, float>>& terms, std::size_t requested_topk,
        std::vector<std::pair<float, DocID>>& ordered, std::size_t& total_hits,
        F5SearchProfile* profile) const;
    [[nodiscard]] const std::vector<Posting>* keyword_postings(std::string_view term) const noexcept;
    [[nodiscard]] const Document* document_at(DocID doc_id) const noexcept;
    [[nodiscard]] std::uint32_t doc_length_for(DocID doc_id) const noexcept;
    [[nodiscard]] bool build_f5_daat_terms(
        const std::vector<std::pair<std::string_view, float>>& query_terms, bool require_all_terms,
        std::vector<F5DaatTermState>& terms, F5SearchProfile* profile) const;
    void sort_f5_daat_results(std::vector<std::pair<float, DocID>>& ordered) const;
    [[nodiscard]] bool daat_wand_or_topk(
        std::vector<F5DaatTermState>& terms, std::size_t requested_topk,
        std::vector<std::pair<float, DocID>>& ordered, std::size_t& total_hits, F5SearchProfile* profile) const;
    [[nodiscard]] bool daat_wand_and_topk(
        std::vector<F5DaatTermState>& terms, std::size_t requested_topk,
        std::vector<std::pair<float, DocID>>& ordered, std::size_t& total_hits, F5SearchProfile* profile) const;
    [[nodiscard]] bool daat_term_exhausted(const F5DaatTermState& st) const noexcept;
    [[nodiscard]] DocID daat_term_current_doc(const F5DaatTermState& st) const noexcept;
    void daat_term_advance(F5DaatTermState& st) const noexcept;
    void daat_term_skip_to(F5DaatTermState& st, DocID target) const noexcept;
    [[nodiscard]] std::size_t count_or_union_docs(std::vector<F5DaatTermState>& terms) const;
    [[nodiscard]] std::unordered_map<std::string, int> compute_f4_term_counts_for_year(int year) const;
    [[nodiscard]] std::vector<F4HotEntry> compute_f4_top10_for_year(int year) const;

    std::vector<std::unique_ptr<LocalIndex>> local_storage_;
    std::vector<Document> forward_index_;
    FlatMap<std::string_view, std::vector<Posting>> author_global_;
    FlatMap<std::string_view, std::vector<Posting>> title_exact_global_;
    FlatMap<std::string_view, std::vector<Posting>> keyword_global_;
    FlatMap<std::string_view, std::vector<std::uint32_t>> f1_author_trigram_ids_;
    std::vector<std::string_view> f1_author_lexicon_;
    std::vector<std::uint32_t> f1_author_doc_counts_;
    std::uint32_t f1_author_max_doc_count_ = 1;
    mutable std::unordered_map<std::string, std::vector<std::pair<std::uint32_t, float>>> f1_author_fuzzy_cache_;
    mutable std::deque<std::string> f1_author_fuzzy_cache_fifo_;
    FlatMap<std::string_view, std::vector<std::uint32_t>> f5_prefix_term_ids_;
    FlatMap<std::string_view, std::vector<std::uint32_t>> f5_trigram_term_ids_;
    FlatMap<std::string_view, std::vector<F5PostingBlockMeta>> f5_term_block_meta_;
    std::vector<std::string_view> f5_term_lexicon_;
    std::vector<std::uint32_t> f5_term_df_;
    std::uint32_t f5_max_term_df_ = 1;
    mutable std::unordered_map<std::string, const std::vector<Posting>*> f5_hot_term_postings_cache_;
    mutable std::deque<std::string> f5_hot_term_postings_cache_fifo_;
    mutable std::unordered_map<std::string, F5ResultCacheEntry> f5_result_cache_;
    mutable std::deque<std::string> f5_result_cache_fifo_;
    mutable std::unordered_map<std::string, std::vector<std::pair<std::uint32_t, float>>> f5_fuzzy_cache_;
    mutable std::deque<std::string> f5_fuzzy_cache_fifo_;

    float avg_dl_ = 0.0f;

    mutable StringArena query_arena_;
    mutable StringArena index_norm_arena_;
    StringArena serving_arena_{16u * 1024u * 1024u};
    StringArena segment_term_arena_{16u * 1024u * 1024u};
    mutable std::vector<float> scoring_board_;
    std::vector<std::pair<std::size_t, std::string_view>> f3_top100_cache_;
    std::unordered_map<int, std::vector<DocID>> year_doc_index_;
    std::unordered_map<int, std::unordered_map<std::string_view, int>> f4_year_term_cache_;
    StringArena f4_term_arena_{4u * 1024u * 1024u};
    std::unordered_map<std::string_view, std::string_view> f4_term_intern_;
    mutable std::unordered_map<int, std::vector<F4HotEntry>> f4_top10_cache_;
    mutable F5SearchProfile* active_profile_ = nullptr;

    static constexpr float k_bm25_k1 = 1.2f;
    static constexpr float k_bm25_b = 0.75f;
    static constexpr std::size_t k_f5_prefix_index_len = 4;
    static constexpr std::size_t k_f1_author_fuzzy_cache_cap = 2048;
    static constexpr std::size_t k_f5_hot_term_postings_cache_cap = 16384;
    static constexpr std::size_t k_f5_result_cache_cap = 256;
    static constexpr std::size_t k_f5_result_cache_doc_cap = 4000;
    static constexpr std::size_t k_f5_fuzzy_cache_cap = 4096;
};
