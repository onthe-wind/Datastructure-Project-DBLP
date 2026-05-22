#include "ExtremeEngine.h"

#include "Analyzer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <ostream>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

namespace {

[[nodiscard]] std::string_view trim_sv(std::string_view s) noexcept {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) {
        s.remove_suffix(1);
    }
    return s;
}

void for_each_author_segment(std::string_view authors, auto&& fn) {
    std::size_t i = 0;
    while (i < authors.size()) {
        const std::size_t j = authors.find("| ", i);
        if (j == std::string_view::npos) {
            fn(trim_sv(std::string_view(authors.data() + i, authors.size() - i)));
            break;
        }
        fn(trim_sv(std::string_view(authors.data() + i, j - i)));
        i = j + 2;
    }
}

[[nodiscard]] std::string_view normalized_span(StringArena& arena, std::string_view raw) {
    const std::vector<std::string_view> toks = Analyzer::normalize_and_tokenize(raw, arena);
    if (toks.empty()) {
        return {};
    }
    const char* const beg = toks.front().data();
    const char* const ed = toks.back().data() + toks.back().size();
    return {beg, static_cast<std::size_t>(ed - beg)};
}

[[nodiscard]] std::filesystem::path locate_f4_config_file(std::string_view file_name) {
    namespace fs = std::filesystem;
    const fs::path file_path{std::string(file_name)};
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

[[nodiscard]] std::filesystem::path locate_f5_segment_file_path() {
    namespace fs = std::filesystem;
    const fs::path file_name = "f5_keyword.seg";
    const fs::path rel = fs::path("segments") / file_name;
    std::error_code cwd_ec;
    const fs::path cwd = fs::current_path(cwd_ec);
    const std::array<fs::path, 5> candidates = {
        cwd_ec ? rel : cwd / rel,
        cwd_ec ? fs::path("build") / rel : cwd.parent_path() / "build" / rel,
        cwd_ec ? fs::path("..") / "build" / rel : cwd.parent_path() / rel,
        fs::path(rel),
        fs::path("cache") / rel,
    };
    for (const fs::path& p : candidates) {
        std::error_code ec;
        if (fs::exists(p, ec) && !ec) {
            return p;
        }
    }
    return candidates.front();
}

[[nodiscard]] std::filesystem::path locate_serving_segment_file_path() {
    namespace fs = std::filesystem;
    const fs::path file_name = "serving_index.seg";
    const fs::path rel = fs::path("segments") / file_name;
    std::error_code cwd_ec;
    const fs::path cwd = fs::current_path(cwd_ec);
    const std::array<fs::path, 5> candidates = {
        cwd_ec ? rel : cwd / rel,
        cwd_ec ? fs::path("build") / rel : cwd.parent_path() / "build" / rel,
        cwd_ec ? fs::path("..") / "build" / rel : cwd.parent_path() / rel,
        fs::path(rel),
        fs::path("cache") / rel,
    };
    for (const fs::path& p : candidates) {
        std::error_code ec;
        if (fs::exists(p, ec) && !ec) {
            return p;
        }
    }
    return candidates.front();
}

void write_varuint32(std::ostream& os, std::uint32_t v) {
    while (v >= 0x80u) {
        const unsigned char b = static_cast<unsigned char>((v & 0x7Fu) | 0x80u);
        os.put(static_cast<char>(b));
        v >>= 7u;
    }
    os.put(static_cast<char>(static_cast<unsigned char>(v)));
}

void write_varuint64(std::ostream& os, std::uint64_t v) {
    while (v >= 0x80u) {
        const unsigned char b = static_cast<unsigned char>((v & 0x7Fu) | 0x80u);
        os.put(static_cast<char>(b));
        v >>= 7u;
    }
    os.put(static_cast<char>(static_cast<unsigned char>(v)));
}

[[nodiscard]] bool read_varuint32(std::istream& is, std::uint32_t& out) {
    out = 0;
    int shift = 0;
    for (int i = 0; i < 5; ++i) {
        const int ch = is.get();
        if (ch == std::char_traits<char>::eof()) {
            return false;
        }
        const std::uint32_t byte = static_cast<std::uint32_t>(static_cast<unsigned char>(ch));
        out |= (byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0u) {
            return true;
        }
        shift += 7;
    }
    return false;
}

[[nodiscard]] bool read_varuint64(std::istream& is, std::uint64_t& out) {
    out = 0;
    int shift = 0;
    for (int i = 0; i < 10; ++i) {
        const int ch = is.get();
        if (ch == std::char_traits<char>::eof()) {
            return false;
        }
        const std::uint64_t byte = static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
        out |= (byte & 0x7Fu) << shift;
        if ((byte & 0x80u) == 0u) {
            return true;
        }
        shift += 7;
    }
    return false;
}

void write_segment_string(std::ostream& os, std::string_view s) {
    write_varuint32(os, static_cast<std::uint32_t>(std::min<std::size_t>(s.size(), 0xFFFFFFFFu)));
    if (!s.empty()) {
        os.write(s.data(), static_cast<std::streamsize>(s.size()));
    }
}

[[nodiscard]] bool read_segment_string(std::istream& is, StringArena& arena, std::string_view& out) {
    std::uint32_t len = 0;
    if (!read_varuint32(is, len)) {
        return false;
    }
    if (len == 0) {
        out = {};
        return true;
    }
    std::string buf(len, '\0');
    is.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    if (!is) {
        return false;
    }
    out = arena.store(buf);
    return true;
}

template <typename Map>
void write_posting_map(std::ostream& out, const Map& map) {
    write_varuint64(out, static_cast<std::uint64_t>(map.size()));
    map.for_each([&](const std::string_view& key, const std::vector<Posting>& postings) {
        write_segment_string(out, key);
        write_varuint32(out, static_cast<std::uint32_t>(std::min<std::size_t>(postings.size(), 0xFFFFFFFFu)));
        DocID prev_doc = 0;
        for (const Posting& p : postings) {
            write_varuint32(out, static_cast<std::uint32_t>(p.doc_id - prev_doc));
            write_varuint32(out, p.tf);
            prev_doc = p.doc_id;
        }
    });
}

template <typename Map>
[[nodiscard]] bool read_posting_map(std::istream& in, StringArena& arena, Map& map) {
    std::uint64_t count = 0;
    if (!read_varuint64(in, count)) {
        return false;
    }
    map.clear();
    map.reserve_capacity(std::max<std::size_t>(1024, static_cast<std::size_t>(count) * 2));
    for (std::uint64_t i = 0; i < count; ++i) {
        std::string_view key;
        if (!read_segment_string(in, arena, key)) {
            return false;
        }
        std::uint32_t posting_count = 0;
        if (!read_varuint32(in, posting_count)) {
            return false;
        }
        std::vector<Posting> postings;
        postings.reserve(posting_count);
        DocID prev_doc = 0;
        for (std::uint32_t p = 0; p < posting_count; ++p) {
            std::uint32_t doc_delta = 0;
            std::uint32_t tf = 0;
            if (!read_varuint32(in, doc_delta) || !read_varuint32(in, tf)) {
                return false;
            }
            const DocID did = static_cast<DocID>(prev_doc + doc_delta);
            postings.push_back(Posting{did, tf});
            prev_doc = did;
        }
        map[key] = std::move(postings);
    }
    return true;
}

struct F4StopWordRepository {
    std::deque<std::string> owned_strings;
    std::unordered_map<std::string_view, std::uint8_t> lookup;
};

[[nodiscard]] std::string_view f4_intern(F4StopWordRepository& repo, std::string token) {
    repo.owned_strings.push_back(std::move(token));
    const std::string& stable = repo.owned_strings.back();
    return std::string_view(stable.data(), stable.size());
}

void add_f4_stop_word(F4StopWordRepository& repo, std::string token) {
    if (token.empty()) {
        return;
    }
    const std::string_view stable = f4_intern(repo, std::move(token));
    repo.lookup[stable] = 1;
}

void load_default_f4_stop_words(F4StopWordRepository& repo) {
    static constexpr std::array<std::string_view, 28> kBaseStopWords = {
        "a", "about", "an", "and", "as", "at", "be", "by", "for", "from", "in", "into", "is", "it",
        "of", "on", "or", "our", "that", "the", "their", "this", "to", "via", "we", "with", "you", "your"};
    static constexpr std::array<std::string_view, 24> kAcademicStopWords = {
        "algorithm", "algorithms", "analysis", "approach", "approaches", "based", "framework", "frameworks",
        "method", "methods", "model", "models", "new", "novel", "paper", "papers", "research", "results",
        "study", "system", "systems", "toward", "towards", "using"};
    for (const std::string_view token : kBaseStopWords) {
        add_f4_stop_word(repo, std::string(token));
    }
    for (const std::string_view token : kAcademicStopWords) {
        add_f4_stop_word(repo, std::string(token));
    }
}

void load_f4_stop_words_from_file(F4StopWordRepository& repo, const std::filesystem::path& file_path) {
    if (file_path.empty()) {
        return;
    }
    std::ifstream in(file_path);
    if (!in.is_open()) {
        return;
    }

    StringArena parse_arena(8u * 1024u);
    std::string raw_line;
    while (std::getline(in, raw_line)) {
        const std::string_view line = trim_sv(raw_line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        parse_arena.reset();
        const std::vector<std::string_view> tokens = Analyzer::normalize_and_tokenize(line, parse_arena);
        for (const std::string_view token : tokens) {
            add_f4_stop_word(repo, std::string(token));
        }
    }
}

void load_external_f4_stop_words(F4StopWordRepository& repo) {
    load_f4_stop_words_from_file(repo, locate_f4_config_file("scientificstopwords_en.txt"));
}

[[nodiscard]] const F4StopWordRepository& get_f4_stop_word_repository() {
    static const F4StopWordRepository repo = []() {
        F4StopWordRepository built;
        built.lookup.reserve(8192);
        load_default_f4_stop_words(built);
        load_external_f4_stop_words(built);
        return built;
    }();
    return repo;
}

[[nodiscard]] bool is_f4_stop_word(std::string_view token) {
    const F4StopWordRepository& repo = get_f4_stop_word_repository();
    return repo.lookup.find(token) != repo.lookup.end();
}

[[nodiscard]] bool is_f4_all_digits(std::string_view token) noexcept {
    if (token.empty()) {
        return false;
    }
    for (const char ch : token) {
        if (ch < '0' || ch > '9') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_f4_year_like_token(std::string_view token) noexcept {
    if (token.size() != 4 || !is_f4_all_digits(token)) {
        return false;
    }
    int year = 0;
    for (const char ch : token) {
        year = year * 10 + (ch - '0');
    }
    return year >= 1900 && year <= 2099;
}

[[nodiscard]] bool is_f4_template_token(std::string_view token) noexcept {
    static constexpr std::array<std::string_view, 32> kTemplateTokens = {
        "challenge", "challenges", "conference", "conferences", "dataset", "datasets", "demo", "demos", "during",
        "international", "issue", "issues", "part", "parts", "poster", "posters", "proceeding", "proceedings",
        "session", "sessions", "shared", "special", "symposium", "symposia", "task", "tasks", "track", "tracks",
        "tutorial", "tutorials", "volume", "volumes"};
    return std::binary_search(kTemplateTokens.begin(), kTemplateTokens.end(), token);
}

[[nodiscard]] bool is_f4_noise_token(std::string_view token) noexcept {
    return is_f4_all_digits(token) || is_f4_year_like_token(token) || is_f4_template_token(token);
}

[[nodiscard]] bool is_f4_valid_term(std::string_view token) {
    return token.size() > 1 && !is_f4_stop_word(token) && !is_f4_noise_token(token);
}

struct F4PhraseAlias {
    std::array<std::string_view, 4> parts;
    std::size_t size;
    std::string_view canonical;
};

struct F4AliasRepository {
    struct PhraseRule {
        std::vector<std::string_view> parts;
        std::string_view canonical;
    };

    std::deque<std::string> owned_strings;
    std::unordered_map<std::string_view, std::string_view> token_alias;
    std::vector<PhraseRule> phrase_alias;
};

[[nodiscard]] std::string_view f4_intern(F4AliasRepository& repo, std::string token) {
    repo.owned_strings.push_back(std::move(token));
    const std::string& stable = repo.owned_strings.back();
    return std::string_view(stable.data(), stable.size());
}

void add_f4_alias_rule(F4AliasRepository& repo, const std::vector<std::string>& lhs_tokens, std::string rhs_token) {
    if (lhs_tokens.empty() || rhs_token.empty()) {
        return;
    }
    const std::string_view canonical = f4_intern(repo, std::move(rhs_token));
    if (lhs_tokens.size() == 1) {
        const std::string_view lhs = f4_intern(repo, lhs_tokens.front());
        repo.token_alias[lhs] = canonical;
        return;
    }

    F4AliasRepository::PhraseRule incoming;
    incoming.parts.reserve(lhs_tokens.size());
    for (const std::string& token : lhs_tokens) {
        incoming.parts.push_back(f4_intern(repo, token));
    }
    incoming.canonical = canonical;

    for (F4AliasRepository::PhraseRule& existing : repo.phrase_alias) {
        if (existing.parts.size() != incoming.parts.size()) {
            continue;
        }
        if (std::equal(existing.parts.begin(), existing.parts.end(), incoming.parts.begin())) {
            existing.canonical = incoming.canonical;
            return;
        }
    }
    repo.phrase_alias.push_back(std::move(incoming));
}

void load_default_f4_aliases(F4AliasRepository& repo) {
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 22> kTokenAliases = {{
        {"bert", "bert"},
        {"berts", "bert"},
        {"cnn", "cnn"},
        {"cnns", "cnn"},
        {"gan", "gan"},
        {"gans", "gan"},
        {"gcn", "gcn"},
        {"gcns", "gcn"},
        {"gnn", "gnn"},
        {"gnns", "gnn"},
        {"gpt", "gpt"},
        {"gpts", "gpt"},
        {"llm", "llm"},
        {"llms", "llm"},
        {"lstm", "lstm"},
        {"lstms", "lstm"},
        {"nlp", "nlp"},
        {"nlps", "nlp"},
        {"rnn", "rnn"},
        {"rnns", "rnn"},
        {"vae", "vae"},
        {"vaes", "vae"},
    }};
    for (const auto& kv : kTokenAliases) {
        add_f4_alias_rule(repo, {std::string(kv.first)}, std::string(kv.second));
    }

    static constexpr std::array<F4PhraseAlias, 16> kPhraseAliases = {{
        {{{"long", "short", "term", "memory"}}, 4, "lstm"},
        {{{"convolutional", "neural", "network", ""}}, 3, "cnn"},
        {{{"convolutional", "neural", "networks", ""}}, 3, "cnn"},
        {{{"generative", "adversarial", "network", ""}}, 3, "gan"},
        {{{"generative", "adversarial", "networks", ""}}, 3, "gan"},
        {{{"graph", "convolutional", "network", ""}}, 3, "gcn"},
        {{{"graph", "convolutional", "networks", ""}}, 3, "gcn"},
        {{{"graph", "neural", "network", ""}}, 3, "gnn"},
        {{{"graph", "neural", "networks", ""}}, 3, "gnn"},
        {{{"large", "language", "model", ""}}, 3, "llm"},
        {{{"large", "language", "models", ""}}, 3, "llm"},
        {{{"natural", "language", "processing", ""}}, 3, "nlp"},
        {{{"recurrent", "neural", "network", ""}}, 3, "rnn"},
        {{{"recurrent", "neural", "networks", ""}}, 3, "rnn"},
        {{{"variational", "autoencoder", "", ""}}, 2, "vae"},
        {{{"variational", "autoencoders", "", ""}}, 2, "vae"},
    }};
    for (const F4PhraseAlias& rule : kPhraseAliases) {
        std::vector<std::string> lhs_tokens;
        lhs_tokens.reserve(rule.size);
        for (std::size_t i = 0; i < rule.size; ++i) {
            lhs_tokens.emplace_back(rule.parts[i]);
        }
        add_f4_alias_rule(repo, lhs_tokens, std::string(rule.canonical));
    }
}

void load_external_f4_aliases(F4AliasRepository& repo) {
    const std::filesystem::path alias_path = locate_f4_config_file("f4_aliases.txt");
    if (alias_path.empty()) {
        return;
    }

    std::ifstream in(alias_path);
    if (!in.is_open()) {
        return;
    }

    StringArena parse_arena(16u * 1024u);
    std::string raw_line;
    while (std::getline(in, raw_line)) {
        const std::string_view line = trim_sv(raw_line);
        if (line.empty() || line.front() == '#') {
            continue;
        }
        const std::size_t sep = line.find("=>");
        if (sep == std::string_view::npos) {
            continue;
        }
        const std::string_view lhs_raw = trim_sv(line.substr(0, sep));
        const std::string_view rhs_raw = trim_sv(line.substr(sep + 2));
        if (lhs_raw.empty() || rhs_raw.empty()) {
            continue;
        }

        parse_arena.reset();
        const std::vector<std::string_view> lhs_sv = Analyzer::normalize_and_tokenize(lhs_raw, parse_arena);
        if (lhs_sv.empty()) {
            continue;
        }
        const std::vector<std::string_view> rhs_sv = Analyzer::normalize_and_tokenize(rhs_raw, parse_arena);
        if (rhs_sv.size() != 1) {
            continue;
        }

        std::vector<std::string> lhs_tokens;
        lhs_tokens.reserve(lhs_sv.size());
        for (const std::string_view token : lhs_sv) {
            lhs_tokens.emplace_back(token);
        }
        add_f4_alias_rule(repo, lhs_tokens, std::string(rhs_sv.front()));
    }
}

[[nodiscard]] const F4AliasRepository& get_f4_alias_repository() {
    static const F4AliasRepository repo = []() {
        F4AliasRepository built;
        built.token_alias.reserve(128);
        built.phrase_alias.reserve(64);
        load_default_f4_aliases(built);
        load_external_f4_aliases(built);
        std::sort(built.phrase_alias.begin(), built.phrase_alias.end(),
                  [](const F4AliasRepository::PhraseRule& a, const F4AliasRepository::PhraseRule& b) {
                      if (a.parts.size() != b.parts.size()) {
                          return a.parts.size() > b.parts.size();
                      }
                      return a.parts < b.parts;
                  });
        return built;
    }();
    return repo;
}

[[nodiscard]] bool matches_f4_phrase_alias(const std::vector<std::string_view>& terms, std::size_t pos,
                                           const F4AliasRepository::PhraseRule& rule) noexcept {
    if (pos + rule.parts.size() > terms.size()) {
        return false;
    }
    for (std::size_t i = 0; i < rule.parts.size(); ++i) {
        if (terms[pos + i] != rule.parts[i]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string_view canonicalize_f4_alias_token(std::string_view token) {
    const F4AliasRepository& repo = get_f4_alias_repository();
    const auto it = repo.token_alias.find(token);
    return it == repo.token_alias.end() ? token : it->second;
}

void build_f4_canonical_filtered_tokens(const std::vector<std::string_view>& terms,
                                        std::vector<std::string_view>& out_tokens) {
    const F4AliasRepository& repo = get_f4_alias_repository();
    out_tokens.clear();
    out_tokens.reserve(terms.size());
    std::size_t i = 0;
    while (i < terms.size()) {
        bool aliased = false;
        for (const F4AliasRepository::PhraseRule& rule : repo.phrase_alias) {
            if (!matches_f4_phrase_alias(terms, i, rule)) {
                continue;
            }
            if (is_f4_valid_term(rule.canonical)) {
                out_tokens.push_back(rule.canonical);
            }
            i += rule.parts.size();
            aliased = true;
            break;
        }
        if (aliased) {
            continue;
        }

        const std::string_view canonical = canonicalize_f4_alias_token(terms[i]);
        if (is_f4_valid_term(canonical)) {
            out_tokens.push_back(canonical);
        }
        ++i;
    }
}

[[nodiscard]] std::string build_f4_concept_key(std::string_view normalized_term) {
    std::string key;
    key.reserve(normalized_term.size());
    std::size_t i = 0;
    while (i < normalized_term.size()) {
        while (i < normalized_term.size() && normalized_term[i] == ' ') {
            ++i;
        }
        if (i >= normalized_term.size()) {
            break;
        }
        const std::size_t beg = i;
        while (i < normalized_term.size() && normalized_term[i] != ' ') {
            ++i;
        }
        const std::string_view token(normalized_term.data() + beg, i - beg);
        if (token.empty() || is_f4_noise_token(token)) {
            continue;
        }
        if (!key.empty()) {
            key.push_back(' ');
        }
        key.append(token.data(), token.size());
    }
    return key;
}

[[nodiscard]] std::size_t count_tokens_in_normalized_term(std::string_view normalized_term) noexcept {
    std::size_t cnt = 0;
    std::size_t i = 0;
    while (i < normalized_term.size()) {
        while (i < normalized_term.size() && normalized_term[i] == ' ') {
            ++i;
        }
        if (i >= normalized_term.size()) {
            break;
        }
        ++cnt;
        while (i < normalized_term.size() && normalized_term[i] != ' ') {
            ++i;
        }
    }
    return cnt;
}

[[nodiscard]] bool prefer_f4_representative_term(std::string_view candidate_term, int candidate_freq,
                                                 std::string_view current_term, int current_freq) noexcept {
    const std::size_t c_tok = count_tokens_in_normalized_term(candidate_term);
    const std::size_t cur_tok = count_tokens_in_normalized_term(current_term);
    if (c_tok != cur_tok) {
        return c_tok < cur_tok;
    }
    if (candidate_freq != current_freq) {
        return candidate_freq > current_freq;
    }
    if (candidate_term.size() != current_term.size()) {
        return candidate_term.size() < current_term.size();
    }
    return candidate_term < current_term;
}

[[nodiscard]] std::vector<std::string_view> split_f4_normalized_tokens(std::string_view normalized_term) {
    std::vector<std::string_view> tokens;
    tokens.reserve(8);
    std::size_t i = 0;
    while (i < normalized_term.size()) {
        while (i < normalized_term.size() && normalized_term[i] == ' ') {
            ++i;
        }
        if (i >= normalized_term.size()) {
            break;
        }
        const std::size_t beg = i;
        while (i < normalized_term.size() && normalized_term[i] != ' ') {
            ++i;
        }
        tokens.emplace_back(normalized_term.data() + beg, i - beg);
    }
    return tokens;
}

[[nodiscard]] bool is_f4_redundant_concept(std::string_view a, std::string_view b) {
    if (a == b) {
        return true;
    }
    const std::vector<std::string_view> ta = split_f4_normalized_tokens(a);
    const std::vector<std::string_view> tb = split_f4_normalized_tokens(b);
    if (ta.empty() || tb.empty()) {
        return false;
    }

    int intersection = 0;
    for (const std::string_view lhs : ta) {
        for (const std::string_view rhs : tb) {
            if (lhs == rhs) {
                ++intersection;
                break;
            }
        }
    }
    const int min_size = static_cast<int>(std::min(ta.size(), tb.size()));
    if (intersection == min_size && min_size >= 1) {
        return true; // subset-like duplication, e.g. "covid" vs "covid pandemic"
    }
    const int union_size = static_cast<int>(ta.size() + tb.size()) - intersection;
    if (intersection >= 2 && union_size > 0) {
        const double jaccard = static_cast<double>(intersection) / static_cast<double>(union_size);
        if (jaccard >= 0.70) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool contains_phrase(const std::vector<std::string_view>& tokens,
                                   const std::vector<std::string_view>& phrase_parts) noexcept {
    if (phrase_parts.empty() || phrase_parts.size() > tokens.size()) {
        return false;
    }
    const std::size_t m = phrase_parts.size();
    for (std::size_t i = 0; i + m <= tokens.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < m; ++j) {
            if (tokens[i + j] != phrase_parts[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

enum class F5BooleanMode : std::uint8_t { Or = 0, And = 1 };
enum class F5SortMode : std::uint8_t { Relevance = 0, Newest = 1 };

struct F5QueryPlan {
    F5BooleanMode boolean_mode = F5BooleanMode::Or;
    F5SortMode sort_mode = F5SortMode::Relevance;
    bool fuzzy_enabled = true;
    int fuzzy_max_edits = 2;
    std::size_t fuzzy_max_expansions = 4;
    std::size_t limit = 0;  // 0 means no limit
    std::size_t offset = 0;
    std::size_t page = 0;   // 1-based, 0 means disabled
    std::size_t size = 0;   // page size, 0 means unset
    std::string terms_query;
    std::vector<std::vector<std::string>> phrases;
    std::vector<std::string> prefix_terms;
    std::vector<std::string> substring_terms;
};

[[nodiscard]] std::string lowercase_ascii_copy(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (const char ch : s) {
        const unsigned char uc = static_cast<unsigned char>(ch);
        if (uc >= 'A' && uc <= 'Z') {
            out.push_back(static_cast<char>(uc - 'A' + 'a'));
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

[[nodiscard]] bool starts_with_sv(std::string_view text, std::string_view prefix) noexcept {
    return prefix.size() <= text.size() && text.substr(0, prefix.size()) == prefix;
}

[[nodiscard]] bool ends_with_sv(std::string_view text, std::string_view suffix) noexcept {
    return suffix.size() <= text.size() && text.substr(text.size() - suffix.size()) == suffix;
}

[[nodiscard]] bool contains_sv(std::string_view text, std::string_view needle) noexcept {
    return text.find(needle) != std::string_view::npos;
}

[[nodiscard]] std::vector<std::string_view> build_trigrams(std::string_view token) {
    std::vector<std::string_view> grams;
    if (token.size() < 3) {
        return grams;
    }
    grams.reserve(token.size() - 2);
    for (std::size_t i = 0; i + 2 < token.size(); ++i) {
        grams.push_back(token.substr(i, 3));
    }
    return grams;
}

[[nodiscard]] std::vector<std::string_view> unique_sorted_terms(const std::vector<std::string_view>& in) {
    std::vector<std::string_view> out = in;
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

[[nodiscard]] bool is_f5_valid_control_token(std::string_view token) {
    if (token.empty()) {
        return false;
    }
    if (token == "*" || token == "~" || token == "\"" || token == ":" || token == "mode" || token == "sort" ||
        token == "fuzzy" || token == "fuzzyexp") {
        return false;
    }
    return true;
}

[[nodiscard]] bool parse_f5_nonnegative_int_arg(std::string_view token, std::string_view prefix, std::size_t& out_value) {
    if (!starts_with_sv(token, prefix)) {
        return false;
    }
    const std::string_view n = token.substr(prefix.size());
    if (n.empty()) {
        return false;
    }
    std::size_t value = 0;
    for (const char ch : n) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        const std::size_t digit = static_cast<std::size_t>(ch - '0');
        if (value > (std::numeric_limits<std::size_t>::max() - digit) / 10u) {
            return false;
        }
        value = value * 10u + digit;
    }
    out_value = value;
    return true;
}

struct F1AuthorQueryPlan {
    bool fuzzy_enabled = true;
    int fuzzy_max_edits = 2;
    std::string author_query;
};

[[nodiscard]] F1AuthorQueryPlan parse_f1_author_query_plan(std::string_view raw_query) {
    F1AuthorQueryPlan plan;
    std::size_t i = 0;
    while (i < raw_query.size()) {
        while (i < raw_query.size() && std::isspace(static_cast<unsigned char>(raw_query[i])) != 0) {
            ++i;
        }
        if (i >= raw_query.size()) {
            break;
        }
        const std::size_t beg = i;
        while (i < raw_query.size() && std::isspace(static_cast<unsigned char>(raw_query[i])) == 0) {
            ++i;
        }
        const std::string token(raw_query.substr(beg, i - beg));
        if (token.empty()) {
            continue;
        }
        const std::string lower = lowercase_ascii_copy(token);
        if (lower == "fuzzy:off") {
            plan.fuzzy_enabled = false;
            continue;
        }
        if (lower == "fuzzy:on") {
            plan.fuzzy_enabled = true;
            continue;
        }
        std::size_t numeric_arg = 0;
        if (parse_f5_nonnegative_int_arg(lower, "fuzzy:", numeric_arg)) {
            plan.fuzzy_enabled = true;
            plan.fuzzy_max_edits = static_cast<int>(std::min<std::size_t>(2, numeric_arg));
            continue;
        }
        if (!plan.author_query.empty()) {
            plan.author_query.push_back(' ');
        }
        plan.author_query.append(token);
    }
    return plan;
}

[[nodiscard]] int bounded_levenshtein_distance(std::string_view a, std::string_view b, int max_dist) {
    if (max_dist < 0) {
        return max_dist + 1;
    }
    const int n = static_cast<int>(a.size());
    const int m = static_cast<int>(b.size());
    if (std::abs(n - m) > max_dist) {
        return max_dist + 1;
    }
    if (n == 0) {
        return m;
    }
    if (m == 0) {
        return n;
    }

    std::vector<int> prev(m + 1);
    std::vector<int> cur(m + 1);
    for (int j = 0; j <= m; ++j) {
        prev[j] = j;
    }

    for (int i = 1; i <= n; ++i) {
        cur[0] = i;
        int row_min = cur[0];
        for (int j = 1; j <= m; ++j) {
            const int cost = (a[static_cast<std::size_t>(i - 1)] == b[static_cast<std::size_t>(j - 1)]) ? 0 : 1;
            const int del = prev[j] + 1;
            const int ins = cur[j - 1] + 1;
            const int sub = prev[j - 1] + cost;
            cur[j] = std::min(del, std::min(ins, sub));
            row_min = std::min(row_min, cur[j]);
        }
        if (row_min > max_dist) {
            return max_dist + 1;
        }
        prev.swap(cur);
    }
    return prev[m];
}

[[nodiscard]] float fuzzy_distance_boost(int dist) noexcept {
    if (dist <= 0) {
        return 1.0f;
    }
    if (dist == 1) {
        return 0.72f;
    }
    if (dist == 2) {
        return 0.45f;
    }
    return 0.25f;
}

[[nodiscard]] F5QueryPlan parse_f5_query_plan(std::string_view raw_query) {
    F5QueryPlan plan;
    bool explicit_mode = false;
    bool seen_and = false;
    bool seen_or = false;

    // 1) Extract quoted phrases for phrase boost.
    StringArena phrase_arena(32u * 1024u);
    std::size_t i = 0;
    while (i < raw_query.size()) {
        if (raw_query[i] != '"') {
            ++i;
            continue;
        }
        const std::size_t beg = i + 1;
        const std::size_t ed = raw_query.find('"', beg);
        if (ed == std::string_view::npos) {
            break;
        }
        const std::string_view phrase_raw = trim_sv(raw_query.substr(beg, ed - beg));
        if (!phrase_raw.empty()) {
            const std::vector<std::string_view> phrase_terms = Analyzer::normalize_and_tokenize(phrase_raw, phrase_arena);
            if (phrase_terms.size() >= 2) {
                std::vector<std::string> phrase;
                phrase.reserve(phrase_terms.size());
                for (const std::string_view t : phrase_terms) {
                    phrase.emplace_back(t);
                }
                plan.phrases.push_back(std::move(phrase));
            }
        }
        i = ed + 1;
    }

    // 2) Parse control directives + sanitize the term query text.
    i = 0;
    while (i < raw_query.size()) {
        while (i < raw_query.size() && std::isspace(static_cast<unsigned char>(raw_query[i])) != 0) {
            ++i;
        }
        if (i >= raw_query.size()) {
            break;
        }
        const std::size_t beg = i;
        while (i < raw_query.size() && std::isspace(static_cast<unsigned char>(raw_query[i])) == 0) {
            ++i;
        }
        std::string token(raw_query.substr(beg, i - beg));
        token.erase(std::remove(token.begin(), token.end(), '"'), token.end());
        if (token.empty()) {
            continue;
        }

        const std::string lower = lowercase_ascii_copy(token);
        if (lower == "mode:and") {
            plan.boolean_mode = F5BooleanMode::And;
            explicit_mode = true;
            continue;
        }
        if (lower == "mode:or") {
            plan.boolean_mode = F5BooleanMode::Or;
            explicit_mode = true;
            continue;
        }
        if (lower == "sort:newest") {
            plan.sort_mode = F5SortMode::Newest;
            continue;
        }
        if (lower == "sort:relevance") {
            plan.sort_mode = F5SortMode::Relevance;
            continue;
        }
        if (lower == "fuzzy:off") {
            plan.fuzzy_enabled = false;
            continue;
        }
        if (lower == "fuzzy:on") {
            plan.fuzzy_enabled = true;
            continue;
        }
        std::size_t numeric_arg = 0;
        if (parse_f5_nonnegative_int_arg(lower, "fuzzy:", numeric_arg)) {
            plan.fuzzy_enabled = true;
            plan.fuzzy_max_edits = static_cast<int>(std::min<std::size_t>(2, numeric_arg));
            continue;
        }
        if (parse_f5_nonnegative_int_arg(lower, "fuzzyexp:", numeric_arg)) {
            plan.fuzzy_max_expansions = std::max<std::size_t>(1, std::min<std::size_t>(12, numeric_arg));
            continue;
        }
        if (parse_f5_nonnegative_int_arg(lower, "limit:", numeric_arg)) {
            plan.limit = numeric_arg;
            continue;
        }
        if (parse_f5_nonnegative_int_arg(lower, "offset:", numeric_arg)) {
            plan.offset = numeric_arg;
            continue;
        }
        if (parse_f5_nonnegative_int_arg(lower, "page:", numeric_arg)) {
            plan.page = numeric_arg;
            continue;
        }
        if (parse_f5_nonnegative_int_arg(lower, "size:", numeric_arg)) {
            plan.size = numeric_arg;
            continue;
        }
        if (lower == "and") {
            seen_and = true;
            continue;
        }
        if (lower == "or") {
            seen_or = true;
            continue;
        }

        if (!lower.empty() && lower.back() == '*') {
            std::string p = lower.substr(0, lower.size() - 1);
            if (is_f5_valid_control_token(p)) {
                plan.prefix_terms.push_back(std::move(p));
            }
            continue;
        }
        if (lower.size() >= 3 && lower.front() == '~' && lower.back() == '~') {
            std::string sub = lower.substr(1, lower.size() - 2);
            if (is_f5_valid_control_token(sub)) {
                plan.substring_terms.push_back(std::move(sub));
            }
            continue;
        }

        if (!plan.terms_query.empty()) {
            plan.terms_query.push_back(' ');
        }
        plan.terms_query.append(token);
    }

    if (!explicit_mode) {
        if (seen_and && !seen_or) {
            plan.boolean_mode = F5BooleanMode::And;
        } else {
            plan.boolean_mode = F5BooleanMode::Or;
        }
    }
    return plan;
}

[[nodiscard]] bool contains_phrase(const std::vector<std::string_view>& tokens,
                                   const std::vector<std::string>& phrase_parts) noexcept {
    if (phrase_parts.empty() || phrase_parts.size() > tokens.size()) {
        return false;
    }
    const std::size_t m = phrase_parts.size();
    for (std::size_t i = 0; i + m <= tokens.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < m; ++j) {
            if (tokens[i + j] != phrase_parts[j]) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool is_f5_query_stopword(std::string_view token) noexcept {
    if (token.size() <= 1) {
        return true;
    }
    static constexpr std::array<std::string_view, 26> kStopWords = {
        "a", "an", "and", "are", "as", "at", "be", "by", "for", "from", "in", "into", "is",
        "it", "of", "on", "or", "that", "the", "their", "this", "to", "via", "we", "with", "you"};
    return std::binary_search(kStopWords.begin(), kStopWords.end(), token);
}

[[nodiscard]] float f5_high_df_penalty(double df_ratio) noexcept {
    if (df_ratio >= 0.30) {
        return 0.20f;
    }
    if (df_ratio >= 0.15) {
        return 0.40f;
    }
    if (df_ratio >= 0.05) {
        return 0.65f;
    }
    return 1.0f;
}

constexpr bool kF4FastModeDisableEvidence = true;

} // namespace

bool ExtremeEngine::save_serving_index() const {
    namespace fs = std::filesystem;
    const fs::path seg_path = locate_serving_segment_file_path();
    std::error_code ec;
    fs::create_directories(seg_path.parent_path(), ec);
    if (ec) {
        std::cerr << "[WarmStart] 无法创建 segment 目录: " << seg_path.parent_path()
                  << " | " << ec.message() << '\n';
        return false;
    }

    const fs::path tmp_path = seg_path.string() + ".tmp";
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        std::cerr << "[WarmStart] 无法打开 segment 临时文件写入: " << tmp_path << '\n';
        return false;
    }

    static constexpr std::array<char, 8> kMagic = {'D', 'B', 'L', 'P', 'S', 'V', '2', '\0'};
    const std::uint32_t version = 1u;
    out.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
    out.write(reinterpret_cast<const char*>(&version), sizeof(version));
    out.write(reinterpret_cast<const char*>(&avg_dl_), sizeof(avg_dl_));
    write_varuint64(out, static_cast<std::uint64_t>(forward_index_.size()));

    for (const Document& doc : forward_index_) {
        write_varuint32(out, doc.id);
        write_segment_string(out, doc.title);
        write_segment_string(out, doc.authors);
        write_segment_string(out, doc.journal);
        out.write(reinterpret_cast<const char*>(&doc.year), sizeof(doc.year));
        write_varuint32(out, doc.doc_length);
        write_segment_string(out, doc.volume);
        write_segment_string(out, doc.month);
        write_segment_string(out, doc.cdrom);
        write_segment_string(out, doc.ee);
        write_segment_string(out, doc.url);
    }

    write_posting_map(out, author_global_);
    write_posting_map(out, title_exact_global_);
    write_posting_map(out, keyword_global_);

    write_varuint64(out, static_cast<std::uint64_t>(f5_term_block_meta_.size()));
    f5_term_block_meta_.for_each([&](const std::string_view& key, const std::vector<F5PostingBlockMeta>& blocks) {
        write_segment_string(out, key);
        write_varuint32(out, static_cast<std::uint32_t>(std::min<std::size_t>(blocks.size(), 0xFFFFFFFFu)));
        for (const F5PostingBlockMeta& b : blocks) {
            write_varuint32(out, b.end_doc);
            write_varuint32(out, b.max_tf);
        }
    });

    out.flush();
    out.close();
    if (!out) {
        std::cerr << "[WarmStart] 写入 serving segment 失败: " << tmp_path << '\n';
        return false;
    }

    fs::rename(tmp_path, seg_path, ec);
    if (ec) {
        fs::remove(seg_path, ec);
        ec.clear();
        fs::rename(tmp_path, seg_path, ec);
        if (ec) {
            fs::remove(tmp_path, ec);
            std::cerr << "[WarmStart] 无法替换 serving segment: " << seg_path << '\n';
            return false;
        }
    }
    std::cout << "[WarmStart] serving segment 已保存: " << seg_path << '\n';
    return true;
}

const std::vector<Posting>* ExtremeEngine::keyword_postings(const std::string_view term) const noexcept {
    return keyword_global_.find(term);
}

const Document* ExtremeEngine::document_at(const DocID doc_id) const noexcept {
    if (doc_id >= forward_index_.size()) {
        return nullptr;
    }
    return &forward_index_[doc_id];
}

std::uint32_t ExtremeEngine::doc_length_for(const DocID doc_id) const noexcept {
    const Document* doc = document_at(doc_id);
    if (doc == nullptr) {
        return 1u;
    }
    return std::max<std::uint32_t>(1u, doc->doc_length);
}

bool ExtremeEngine::try_load_serving_index() {
    namespace fs = std::filesystem;
    const fs::path seg_path = locate_serving_segment_file_path();
    std::error_code ec;
    if (!fs::exists(seg_path, ec) || ec) {
        return false;
    }

    std::ifstream in(seg_path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }

    std::array<char, 8> magic{};
    in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!in || std::string_view(magic.data(), magic.size()) != std::string_view("DBLPSV2\0", 8)) {
        return false;
    }

    std::uint32_t version = 0;
    float loaded_avg_dl = 0.0f;
    in.read(reinterpret_cast<char*>(&version), sizeof(version));
    in.read(reinterpret_cast<char*>(&loaded_avg_dl), sizeof(loaded_avg_dl));
    if (!in || version != 1u) {
        return false;
    }

    serving_arena_.reset();
    segment_term_arena_.reset();
    local_storage_.clear();
    forward_index_.clear();
    author_global_.clear();
    title_exact_global_.clear();
    keyword_global_.clear();
    f5_term_block_meta_.clear();

    std::uint64_t doc_count = 0;
    if (!read_varuint64(in, doc_count)) {
        return false;
    }
    if (doc_count > static_cast<std::uint64_t>(std::numeric_limits<DocID>::max())) {
        return false;
    }
    forward_index_.reserve(static_cast<std::size_t>(doc_count));
    for (std::uint64_t i = 0; i < doc_count; ++i) {
        Document doc;
        std::uint32_t id = 0;
        if (!read_varuint32(in, id)) {
            return false;
        }
        doc.id = id;
        if (!read_segment_string(in, serving_arena_, doc.title) ||
            !read_segment_string(in, serving_arena_, doc.authors) ||
            !read_segment_string(in, serving_arena_, doc.journal)) {
            return false;
        }
        in.read(reinterpret_cast<char*>(&doc.year), sizeof(doc.year));
        if (!read_varuint32(in, doc.doc_length)) {
            return false;
        }
        if (!read_segment_string(in, serving_arena_, doc.volume) ||
            !read_segment_string(in, serving_arena_, doc.month) ||
            !read_segment_string(in, serving_arena_, doc.cdrom) ||
            !read_segment_string(in, serving_arena_, doc.ee) ||
            !read_segment_string(in, serving_arena_, doc.url)) {
            return false;
        }
        if (!in) {
            return false;
        }
        forward_index_.push_back(doc);
    }

    if (!read_posting_map(in, serving_arena_, author_global_) ||
        !read_posting_map(in, serving_arena_, title_exact_global_) ||
        !read_posting_map(in, segment_term_arena_, keyword_global_)) {
        return false;
    }

    std::uint64_t block_term_count = 0;
    if (!read_varuint64(in, block_term_count)) {
        return false;
    }
    f5_term_block_meta_.reserve_capacity(std::max<std::size_t>(1024, static_cast<std::size_t>(block_term_count) * 2));
    for (std::uint64_t i = 0; i < block_term_count; ++i) {
        std::string_view key;
        if (!read_segment_string(in, segment_term_arena_, key)) {
            return false;
        }
        std::uint32_t block_count = 0;
        if (!read_varuint32(in, block_count)) {
            return false;
        }
        std::vector<F5PostingBlockMeta> blocks;
        blocks.reserve(block_count);
        for (std::uint32_t b = 0; b < block_count; ++b) {
            std::uint32_t end_doc = 0;
            std::uint32_t max_tf = 0;
            if (!read_varuint32(in, end_doc) || !read_varuint32(in, max_tf)) {
                return false;
            }
            blocks.push_back(F5PostingBlockMeta{static_cast<DocID>(end_doc), static_cast<std::uint16_t>(max_tf)});
        }
        f5_term_block_meta_[key] = std::move(blocks);
    }

    if (!in) {
        return false;
    }

    avg_dl_ = loaded_avg_dl;
    scoring_board_.assign(forward_index_.size(), 0.0f);
    rebuild_f3_top100_cache();
    rebuild_year_doc_index();
    rebuild_f4_year_term_cache();
    rebuild_f1_author_fuzzy_index();
    rebuild_f5_partial_match_index();
    f4_top10_cache_.clear();
    return !forward_index_.empty();
}

void ExtremeEngine::merge_local_indexes(std::vector<std::unique_ptr<LocalIndex>> locals) {
    local_storage_ = std::move(locals);
    forward_index_.clear();
    author_global_.clear();
    title_exact_global_.clear();
    keyword_global_.clear();
    f1_author_trigram_ids_.clear();
    f1_author_lexicon_.clear();
    f1_author_doc_counts_.clear();
    f1_author_max_doc_count_ = 1;
    f1_author_fuzzy_cache_.clear();
    f1_author_fuzzy_cache_fifo_.clear();
    f5_prefix_term_ids_.clear();
    f5_trigram_term_ids_.clear();
    f5_term_block_meta_.clear();
    f5_term_lexicon_.clear();
    f5_term_df_.clear();
    f5_max_term_df_ = 1;
    f5_hot_term_postings_cache_.clear();
    f5_hot_term_postings_cache_fifo_.clear();
    f5_result_cache_.clear();
    f5_result_cache_fifo_.clear();
    f5_fuzzy_cache_.clear();
    f5_fuzzy_cache_fifo_.clear();

    author_global_.reserve_capacity(static_cast<std::size_t>(1) << 23);
    title_exact_global_.reserve_capacity(static_cast<std::size_t>(1) << 23);
    keyword_global_.reserve_capacity(static_cast<std::size_t>(1) << 25);

    std::size_t expected_docs = 0;
    for (const auto& li : local_storage_) {
        if (li) {
            expected_docs += li->forward_index.size();
        }
    }
    const bool keyword_segment_loaded = try_load_f5_keyword_segment(expected_docs);

    std::uint64_t dl_sum = 0;

    for (auto& li : local_storage_) {
        if (!li) {
            continue;
        }
        const DocID base = static_cast<DocID>(forward_index_.size());

        for (const Document& d : li->forward_index) {
            Document dg = d;
            dg.id = static_cast<DocID>(base + d.id);
            forward_index_.push_back(dg);
            dl_sum += static_cast<std::uint64_t>(d.doc_length);
        }

        li->author_inverted.for_each([&](const std::string_view& key, const std::vector<Posting>& vec) {
            std::vector<Posting>& dest = author_global_[key];
            for (const Posting& p : vec) {
                dest.push_back(Posting{static_cast<DocID>(p.doc_id + base), p.tf});
            }
        });
        li->title_exact_inverted.for_each([&](const std::string_view& key, const std::vector<Posting>& vec) {
            std::vector<Posting>& dest = title_exact_global_[key];
            for (const Posting& p : vec) {
                dest.push_back(Posting{static_cast<DocID>(p.doc_id + base), p.tf});
            }
        });
        if (!keyword_segment_loaded) {
            li->keyword_inverted.for_each([&](const std::string_view& key, const std::vector<Posting>& vec) {
                std::vector<Posting>& dest = keyword_global_[key];
                for (const Posting& p : vec) {
                    dest.push_back(Posting{static_cast<DocID>(p.doc_id + base), p.tf});
                }
            });
        }
    }
    if (!keyword_segment_loaded) {
        save_f5_keyword_segment(forward_index_.size());
    }
    rebuild_f3_top100_cache();
    rebuild_year_doc_index();
    rebuild_f4_year_term_cache();
    rebuild_f1_author_fuzzy_index();
    rebuild_f5_partial_match_index();
    f4_top10_cache_.clear();

    const std::size_t n = forward_index_.size();
    avg_dl_ = n > 0 ? static_cast<float>(static_cast<double>(dl_sum) / static_cast<double>(n)) : 0.0f;
    scoring_board_.assign(n, 0.0f);
    if (!save_serving_index()) {
        std::cerr << "[WarmStart] 警告: serving segment 保存失败，下次启动仍会回退到 XML 构建。\n";
    }
}

void ExtremeEngine::rebuild_author_and_title_inverted_from_forward() {
    author_global_.clear();
    title_exact_global_.clear();
    index_norm_arena_.reset();

    const std::size_t hint = std::max<std::size_t>(8, forward_index_.size() * 8 + 16);
    author_global_.reserve(hint);
    title_exact_global_.reserve(hint);

    for (const Document& d : forward_index_) {
        if (!d.title.empty()) {
            const std::string_view ts = normalized_span(index_norm_arena_, d.title);
            if (!ts.empty()) {
                title_exact_global_[ts].push_back(Posting{d.id, 1u});
            }
        }
        if (!d.authors.empty()) {
            for_each_author_segment(d.authors, [&](std::string_view aseg) {
                const std::string_view av = trim_sv(aseg);
                if (av.empty()) {
                    return;
                }
                const std::string_view nk = normalized_span(index_norm_arena_, av);
                if (nk.empty()) {
                    return;
                }
                author_global_[nk].push_back(Posting{d.id, 1u});
            });
        }
    }
}

void ExtremeEngine::rebuild_f1_author_fuzzy_index() {
    f1_author_trigram_ids_.clear();
    f1_author_lexicon_.clear();
    f1_author_doc_counts_.clear();
    f1_author_max_doc_count_ = 1;
    f1_author_fuzzy_cache_.clear();
    f1_author_fuzzy_cache_fifo_.clear();

    if (author_global_.empty()) {
        return;
    }

    f1_author_lexicon_.reserve(author_global_.size());
    std::unordered_map<std::string_view, std::uint32_t> author_df_tmp;
    author_df_tmp.reserve(author_global_.size() * 2 + 1);
    author_global_.for_each([&](std::string_view author, const std::vector<Posting>& postings) {
        if (author.empty()) {
            return;
        }
        f1_author_lexicon_.push_back(author);
        const std::uint32_t cnt = static_cast<std::uint32_t>(std::min<std::size_t>(postings.size(), 0xFFFFFFFFu));
        author_df_tmp[author] = cnt;
        f1_author_max_doc_count_ = std::max(f1_author_max_doc_count_, std::max<std::uint32_t>(1u, cnt));
    });
    std::sort(f1_author_lexicon_.begin(), f1_author_lexicon_.end());
    f1_author_lexicon_.erase(std::unique(f1_author_lexicon_.begin(), f1_author_lexicon_.end()), f1_author_lexicon_.end());

    f1_author_doc_counts_.assign(f1_author_lexicon_.size(), 1u);
    for (std::size_t i = 0; i < f1_author_lexicon_.size(); ++i) {
        const auto it = author_df_tmp.find(f1_author_lexicon_[i]);
        if (it != author_df_tmp.end()) {
            f1_author_doc_counts_[i] = std::max<std::uint32_t>(1u, it->second);
        }
    }

    f1_author_trigram_ids_.reserve_capacity(std::max<std::size_t>(1024, f1_author_lexicon_.size() * 3));
    for (std::uint32_t i = 0; i < f1_author_lexicon_.size(); ++i) {
        const std::string_view author = f1_author_lexicon_[i];
        if (author.size() < 3) {
            continue;
        }
        std::vector<std::string_view> grams = build_trigrams(author);
        grams = unique_sorted_terms(grams);
        for (const std::string_view g : grams) {
            f1_author_trigram_ids_[g].push_back(i);
        }
    }
}

void ExtremeEngine::collect_f1_fuzzy_author_candidates(
    std::string_view typo, int max_edits, std::vector<std::pair<std::string_view, float>>& out_authors) const {
    out_authors.clear();
    if (typo.empty() || typo.size() < 3 || f1_author_lexicon_.empty()) {
        return;
    }
    max_edits = std::min(2, std::max(0, max_edits));
    const int adaptive_max_edits = static_cast<int>(typo.size()) <= 6 ? std::min(1, max_edits) : max_edits;
    if (adaptive_max_edits <= 0) {
        return;
    }

    auto hydrate_from_cache = [&](const std::vector<std::pair<std::uint32_t, float>>& cache) {
        out_authors.reserve(cache.size());
        for (const auto& hit : cache) {
            if (hit.first >= f1_author_lexicon_.size()) {
                continue;
            }
            out_authors.push_back({f1_author_lexicon_[hit.first], hit.second});
        }
    };
    std::string cache_key;
    cache_key.reserve(typo.size() + 6);
    cache_key.push_back(static_cast<char>('0' + adaptive_max_edits));
    cache_key.push_back(':');
    cache_key.append(typo.data(), typo.size());
    if (const auto it = f1_author_fuzzy_cache_.find(cache_key); it != f1_author_fuzzy_cache_.end()) {
        hydrate_from_cache(it->second);
        return;
    }

    std::vector<std::string_view> grams = build_trigrams(typo);
    grams = unique_sorted_terms(grams);
    if (grams.empty()) {
        return;
    }

    std::vector<const std::vector<std::uint32_t>*> gram_postings;
    gram_postings.reserve(grams.size());
    constexpr std::size_t kMaxGramPostingScan = 180000;
    for (const std::string_view g : grams) {
        const std::vector<std::uint32_t>* ids = f1_author_trigram_ids_.find(g);
        if (ids == nullptr || ids->empty()) {
            continue;
        }
        if (ids->size() <= kMaxGramPostingScan) {
            gram_postings.push_back(ids);
        }
    }
    if (gram_postings.empty()) {
        return;
    }
    std::sort(gram_postings.begin(), gram_postings.end(),
              [](const std::vector<std::uint32_t>* a, const std::vector<std::uint32_t>* b) {
                  return a->size() < b->size();
              });
    constexpr std::size_t kMaxActiveGrams = 5;
    if (gram_postings.size() > kMaxActiveGrams) {
        gram_postings.resize(kMaxActiveGrams);
    }

    std::unordered_map<std::uint32_t, std::uint16_t> overlap_counts;
    overlap_counts.reserve(8192);
    for (const auto* ids : gram_postings) {
        for (const std::uint32_t aid : *ids) {
            ++overlap_counts[aid];
        }
    }
    if (overlap_counts.empty()) {
        return;
    }

    struct Candidate {
        std::uint32_t aid = 0;
        std::uint16_t overlap = 0;
        std::uint16_t len_diff = 0;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(overlap_counts.size());
    const std::size_t typo_len = typo.size();
    const std::uint16_t min_overlap = static_cast<std::uint16_t>(std::max<std::size_t>(1, grams.size() / 3));
    for (const auto& kv : overlap_counts) {
        if (kv.first >= f1_author_lexicon_.size()) {
            continue;
        }
        const std::string_view author = f1_author_lexicon_[kv.first];
        const std::size_t len_diff =
            typo_len >= author.size() ? (typo_len - author.size()) : (author.size() - typo_len);
        if (len_diff > static_cast<std::size_t>(adaptive_max_edits + 3)) {
            continue;
        }
        if (kv.second < min_overlap) {
            continue;
        }
        candidates.push_back(
            Candidate{kv.first, kv.second, static_cast<std::uint16_t>(std::min<std::size_t>(len_diff, 65535u))});
    }
    if (candidates.empty()) {
        return;
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.overlap != b.overlap) {
            return a.overlap > b.overlap;
        }
        if (a.len_diff != b.len_diff) {
            return a.len_diff < b.len_diff;
        }
        return a.aid < b.aid;
    });
    constexpr std::size_t kVerifyCap = 160;
    if (candidates.size() > kVerifyCap) {
        candidates.resize(kVerifyCap);
    }

    const float max_df_log = std::log1p(static_cast<float>(std::max<std::uint32_t>(1u, f1_author_max_doc_count_)));
    std::vector<std::pair<std::uint32_t, float>> cache_hits;
    cache_hits.reserve(16);
    for (const Candidate& c : candidates) {
        const std::string_view author = f1_author_lexicon_[c.aid];
        const int dist = bounded_levenshtein_distance(typo, author, adaptive_max_edits);
        if (dist > adaptive_max_edits) {
            continue;
        }
        const float overlap_ratio = static_cast<float>(c.overlap) / static_cast<float>(grams.size());
        const std::uint32_t prior_cnt = c.aid < f1_author_doc_counts_.size() ? f1_author_doc_counts_[c.aid] : 1u;
        const float prior_ratio = max_df_log > 0.0f ? (std::log1p(static_cast<float>(prior_cnt)) / max_df_log) : 0.0f;
        float boost = fuzzy_distance_boost(dist) * (0.76f + 0.24f * overlap_ratio);
        boost *= (0.88f + 0.24f * std::clamp(prior_ratio, 0.0f, 1.0f));
        if (starts_with_sv(author, typo)) {
            boost *= 1.08f;
        }
        cache_hits.push_back({c.aid, boost});
    }
    if (cache_hits.empty()) {
        return;
    }
    std::sort(cache_hits.begin(), cache_hits.end(),
              [&](const std::pair<std::uint32_t, float>& a, const std::pair<std::uint32_t, float>& b) {
                  if (a.second != b.second) {
                      return a.second > b.second;
                  }
                  const std::string_view aa = a.first < f1_author_lexicon_.size() ? f1_author_lexicon_[a.first] : std::string_view{};
                  const std::string_view bb = b.first < f1_author_lexicon_.size() ? f1_author_lexicon_[b.first] : std::string_view{};
                  return aa < bb;
              });
    constexpr std::size_t kOutCap = 10;
    if (cache_hits.size() > kOutCap) {
        cache_hits.resize(kOutCap);
    }

    while (f1_author_fuzzy_cache_.size() >= k_f1_author_fuzzy_cache_cap && !f1_author_fuzzy_cache_fifo_.empty()) {
        const std::string evict_key = std::move(f1_author_fuzzy_cache_fifo_.front());
        f1_author_fuzzy_cache_fifo_.pop_front();
        f1_author_fuzzy_cache_.erase(evict_key);
    }
    f1_author_fuzzy_cache_fifo_.push_back(cache_key);
    f1_author_fuzzy_cache_[cache_key] = cache_hits;
    hydrate_from_cache(cache_hits);
}

bool ExtremeEngine::try_load_f5_keyword_segment(std::size_t expected_doc_count) {
    namespace fs = std::filesystem;
    keyword_global_.clear();
    f5_term_block_meta_.clear();
    segment_term_arena_.reset();

    const fs::path seg_path = locate_f5_segment_file_path();
    std::error_code ec;
    if (!fs::exists(seg_path, ec) || ec) {
        return false;
    }
    std::ifstream in(seg_path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }

    std::array<char, 8> magic{};
    in.read(magic.data(), static_cast<std::streamsize>(magic.size()));
    if (!in || std::string_view(magic.data(), magic.size()) != std::string_view("F5SEGv1\0", 8)) {
        return false;
    }

    std::uint32_t version = 0;
    std::uint64_t doc_count = 0;
    std::uint64_t term_count = 0;
    std::uint32_t block_size = 0;
    in.read(reinterpret_cast<char*>(&version), sizeof(version));
    in.read(reinterpret_cast<char*>(&doc_count), sizeof(doc_count));
    in.read(reinterpret_cast<char*>(&term_count), sizeof(term_count));
    in.read(reinterpret_cast<char*>(&block_size), sizeof(block_size));
    if (!in || version != 1u || block_size != 128u || doc_count != static_cast<std::uint64_t>(expected_doc_count)) {
        return false;
    }

    keyword_global_.reserve_capacity(std::max<std::size_t>(1024, static_cast<std::size_t>(term_count) * 2));
    f5_term_block_meta_.reserve_capacity(std::max<std::size_t>(1024, static_cast<std::size_t>(term_count) * 2));

    std::string term_buf;
    for (std::uint64_t i = 0; i < term_count; ++i) {
        std::uint32_t term_len = 0;
        if (!read_varuint32(in, term_len)) {
            keyword_global_.clear();
            f5_term_block_meta_.clear();
            return false;
        }
        term_buf.assign(term_len, '\0');
        in.read(term_buf.data(), static_cast<std::streamsize>(term_len));
        if (!in) {
            keyword_global_.clear();
            f5_term_block_meta_.clear();
            return false;
        }
        const std::string_view stable_term = segment_term_arena_.store(term_buf);

        std::uint32_t posting_count = 0;
        if (!read_varuint32(in, posting_count)) {
            keyword_global_.clear();
            f5_term_block_meta_.clear();
            return false;
        }
        std::vector<Posting> postings;
        postings.reserve(posting_count);
        DocID prev_doc = 0;
        for (std::uint32_t p = 0; p < posting_count; ++p) {
            std::uint32_t doc_delta = 0;
            std::uint32_t tf = 0;
            if (!read_varuint32(in, doc_delta) || !read_varuint32(in, tf)) {
                keyword_global_.clear();
                f5_term_block_meta_.clear();
                return false;
            }
            const DocID did = static_cast<DocID>(prev_doc + doc_delta);
            postings.push_back(Posting{did, tf});
            prev_doc = did;
        }
        keyword_global_[stable_term] = std::move(postings);

        std::uint32_t block_count = 0;
        if (!read_varuint32(in, block_count)) {
            keyword_global_.clear();
            f5_term_block_meta_.clear();
            return false;
        }
        std::vector<F5PostingBlockMeta> blocks;
        blocks.reserve(block_count);
        for (std::uint32_t b = 0; b < block_count; ++b) {
            std::uint32_t end_doc = 0;
            std::uint32_t max_tf = 0;
            if (!read_varuint32(in, end_doc) || !read_varuint32(in, max_tf)) {
                keyword_global_.clear();
                f5_term_block_meta_.clear();
                return false;
            }
            blocks.push_back(F5PostingBlockMeta{static_cast<DocID>(end_doc), static_cast<std::uint16_t>(max_tf)});
        }
        if (!blocks.empty()) {
            f5_term_block_meta_[stable_term] = std::move(blocks);
        }
    }

    return true;
}

void ExtremeEngine::save_f5_keyword_segment(std::size_t doc_count) const {
    namespace fs = std::filesystem;
    const fs::path seg_path = locate_f5_segment_file_path();
    std::error_code ec;
    fs::create_directories(seg_path.parent_path(), ec);
    if (ec) {
        return;
    }

    const fs::path tmp_path = seg_path.string() + ".tmp";
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return;
    }

    static constexpr std::array<char, 8> kMagic = {'F', '5', 'S', 'E', 'G', 'v', '1', '\0'};
    const std::uint32_t version = 1u;
    const std::uint64_t n_docs = static_cast<std::uint64_t>(doc_count);
    const std::uint64_t n_terms = static_cast<std::uint64_t>(keyword_global_.size());
    const std::uint32_t block_size = 128u;
    out.write(kMagic.data(), static_cast<std::streamsize>(kMagic.size()));
    out.write(reinterpret_cast<const char*>(&version), sizeof(version));
    out.write(reinterpret_cast<const char*>(&n_docs), sizeof(n_docs));
    out.write(reinterpret_cast<const char*>(&n_terms), sizeof(n_terms));
    out.write(reinterpret_cast<const char*>(&block_size), sizeof(block_size));
    if (!out) {
        return;
    }

    keyword_global_.for_each([&](const std::string_view& term, const std::vector<Posting>& postings) {
        write_varuint32(out, static_cast<std::uint32_t>(std::min<std::size_t>(term.size(), 0xFFFFFFFFu)));
        out.write(term.data(), static_cast<std::streamsize>(term.size()));

        write_varuint32(out, static_cast<std::uint32_t>(std::min<std::size_t>(postings.size(), 0xFFFFFFFFu)));
        DocID prev_doc = 0;
        std::vector<F5PostingBlockMeta> blocks;
        blocks.reserve((postings.size() + block_size - 1u) / block_size);
        std::uint16_t cur_block_max_tf = 0;
        std::size_t cur_block_start = 0;
        for (std::size_t i = 0; i < postings.size(); ++i) {
            const Posting& p = postings[i];
            const std::uint32_t delta = static_cast<std::uint32_t>(p.doc_id - prev_doc);
            write_varuint32(out, delta);
            write_varuint32(out, p.tf);
            prev_doc = p.doc_id;

            if (i == cur_block_start) {
                cur_block_max_tf = static_cast<std::uint16_t>(std::min<std::uint32_t>(p.tf, 0xFFFFu));
            } else {
                cur_block_max_tf = std::max<std::uint16_t>(cur_block_max_tf, static_cast<std::uint16_t>(std::min<std::uint32_t>(p.tf, 0xFFFFu)));
            }
            const bool block_end = ((i + 1u) % block_size == 0u) || (i + 1u == postings.size());
            if (block_end) {
                blocks.push_back(F5PostingBlockMeta{p.doc_id, cur_block_max_tf});
                cur_block_start = i + 1u;
            }
        }

        write_varuint32(out, static_cast<std::uint32_t>(std::min<std::size_t>(blocks.size(), 0xFFFFFFFFu)));
        for (const F5PostingBlockMeta& b : blocks) {
            write_varuint32(out, static_cast<std::uint32_t>(b.end_doc));
            write_varuint32(out, static_cast<std::uint32_t>(b.max_tf));
        }
    });

    out.flush();
    out.close();
    if (!out) {
        return;
    }
    fs::rename(tmp_path, seg_path, ec);
    if (ec) {
        fs::remove(seg_path, ec);
        ec.clear();
        fs::rename(tmp_path, seg_path, ec);
        if (ec) {
            fs::remove(tmp_path, ec);
        }
    }
}

void ExtremeEngine::rebuild_f5_partial_match_index() {
    f5_prefix_term_ids_.clear();
    f5_trigram_term_ids_.clear();
    f5_term_lexicon_.clear();
    f5_term_df_.clear();
    f5_max_term_df_ = 1;
    f5_hot_term_postings_cache_.clear();
    f5_hot_term_postings_cache_fifo_.clear();
    f5_result_cache_.clear();
    f5_result_cache_fifo_.clear();
    f5_fuzzy_cache_.clear();
    f5_fuzzy_cache_fifo_.clear();

    if (keyword_global_.empty()) {
        return;
    }

    f5_term_lexicon_.reserve(keyword_global_.size());
    std::unordered_map<std::string_view, std::uint32_t> term_df_tmp;
    term_df_tmp.reserve(keyword_global_.size() * 2 + 1);
    keyword_global_.for_each([&](std::string_view term, const std::vector<Posting>& postings) {
        if (term.empty()) {
            return;
        }
        f5_term_lexicon_.push_back(term);
        const std::uint32_t df = static_cast<std::uint32_t>(std::min<std::size_t>(postings.size(), 0xFFFFFFFFu));
        term_df_tmp[term] = df;
        f5_max_term_df_ = std::max(f5_max_term_df_, df);
    });
    std::sort(f5_term_lexicon_.begin(), f5_term_lexicon_.end());
    f5_term_lexicon_.erase(std::unique(f5_term_lexicon_.begin(), f5_term_lexicon_.end()), f5_term_lexicon_.end());
    f5_term_df_.assign(f5_term_lexicon_.size(), 1u);
    for (std::size_t i = 0; i < f5_term_lexicon_.size(); ++i) {
        const auto it = term_df_tmp.find(f5_term_lexicon_[i]);
        if (it != term_df_tmp.end()) {
            f5_term_df_[i] = std::max<std::uint32_t>(1u, it->second);
        }
    }

    f5_prefix_term_ids_.reserve_capacity(std::max<std::size_t>(1024, f5_term_lexicon_.size() * 2));
    f5_trigram_term_ids_.reserve_capacity(std::max<std::size_t>(1024, f5_term_lexicon_.size() * 4));

    for (std::uint32_t i = 0; i < f5_term_lexicon_.size(); ++i) {
        const std::string_view term = f5_term_lexicon_[i];
        const std::size_t plen = std::min<std::size_t>(k_f5_prefix_index_len, term.size());
        if (plen > 0) {
            const std::string_view pfx = term.substr(0, plen);
            f5_prefix_term_ids_[pfx].push_back(i);
        }
        if (term.size() >= 3) {
            std::vector<std::string_view> grams = build_trigrams(term);
            grams = unique_sorted_terms(grams);
            for (const std::string_view g : grams) {
                f5_trigram_term_ids_[g].push_back(i);
            }
        }
    }
}

void ExtremeEngine::collect_f5_prefix_candidates(std::string_view prefix, std::vector<std::string_view>& out_terms) const {
    out_terms.clear();
    if (prefix.empty() || f5_term_lexicon_.empty()) {
        return;
    }

    if (prefix.size() < k_f5_prefix_index_len) {
        const auto lo = std::lower_bound(f5_term_lexicon_.begin(), f5_term_lexicon_.end(), prefix);
        for (auto it = lo; it != f5_term_lexicon_.end(); ++it) {
            if (!starts_with_sv(*it, prefix)) {
                break;
            }
            out_terms.push_back(*it);
        }
        return;
    }

    const std::string_view gate = prefix.substr(0, k_f5_prefix_index_len);
    const std::vector<std::uint32_t>* ids = f5_prefix_term_ids_.find(gate);
    if (ids == nullptr) {
        return;
    }
    out_terms.reserve(ids->size());
    for (const std::uint32_t tid : *ids) {
        if (tid >= f5_term_lexicon_.size()) {
            continue;
        }
        const std::string_view term = f5_term_lexicon_[tid];
        if (starts_with_sv(term, prefix)) {
            out_terms.push_back(term);
        }
    }
}

void ExtremeEngine::collect_f5_substring_candidates(std::string_view needle, std::vector<std::string_view>& out_terms) const {
    out_terms.clear();
    if (needle.empty() || f5_term_lexicon_.empty()) {
        return;
    }

    if (needle.size() < 3) {
        for (const std::string_view term : f5_term_lexicon_) {
            if (contains_sv(term, needle)) {
                out_terms.push_back(term);
            }
        }
        return;
    }

    std::vector<std::string_view> grams = build_trigrams(needle);
    grams = unique_sorted_terms(grams);
    if (grams.empty()) {
        return;
    }

    std::vector<const std::vector<std::uint32_t>*> gram_postings;
    gram_postings.reserve(grams.size());
    for (const std::string_view g : grams) {
        const std::vector<std::uint32_t>* ids = f5_trigram_term_ids_.find(g);
        if (ids == nullptr || ids->empty()) {
            return;
        }
        gram_postings.push_back(ids);
    }
    std::sort(gram_postings.begin(), gram_postings.end(),
              [](const std::vector<std::uint32_t>* a, const std::vector<std::uint32_t>* b) {
                  return a->size() < b->size();
              });

    std::vector<std::uint32_t> candidate_ids = *gram_postings.front();
    for (std::size_t gi = 1; gi < gram_postings.size(); ++gi) {
        const std::vector<std::uint32_t>& ids = *gram_postings[gi];
        std::vector<std::uint32_t> inter;
        inter.reserve(std::min(candidate_ids.size(), ids.size()));
        std::size_t a = 0;
        std::size_t b = 0;
        while (a < candidate_ids.size() && b < ids.size()) {
            if (candidate_ids[a] == ids[b]) {
                inter.push_back(candidate_ids[a]);
                ++a;
                ++b;
            } else if (candidate_ids[a] < ids[b]) {
                ++a;
            } else {
                ++b;
            }
        }
        candidate_ids.swap(inter);
        if (candidate_ids.empty()) {
            return;
        }
    }

    for (const std::uint32_t tid : candidate_ids) {
        if (tid >= f5_term_lexicon_.size()) {
            continue;
        }
        const std::string_view term = f5_term_lexicon_[tid];
        if (contains_sv(term, needle)) {
            out_terms.push_back(term);
        }
    }
}

void ExtremeEngine::collect_f5_fuzzy_candidates(
    std::string_view typo, int max_edits, std::vector<std::pair<std::string_view, float>>& out_terms) const {
    out_terms.clear();
    if (typo.empty() || typo.size() < 3 || f5_term_lexicon_.empty()) {
        return;
    }
    if (max_edits < 0) {
        return;
    }
    max_edits = std::min(2, max_edits);
    const int adaptive_max_edits = static_cast<int>(typo.size()) <= 5 ? std::min(1, max_edits) : max_edits;
    if (adaptive_max_edits <= 0) {
        return;
    }

    auto hydrate_out_from_cached = [&](const std::vector<std::pair<std::uint32_t, float>>& cached) {
        out_terms.reserve(cached.size());
        for (const auto& hit : cached) {
            if (hit.first >= f5_term_lexicon_.size()) {
                continue;
            }
            out_terms.push_back({f5_term_lexicon_[hit.first], hit.second});
        }
    };
    std::string cache_key;
    cache_key.reserve(typo.size() + 6);
    cache_key.push_back(static_cast<char>('0' + adaptive_max_edits));
    cache_key.push_back(':');
    cache_key.append(typo.data(), typo.size());
    if (const auto it = f5_fuzzy_cache_.find(cache_key); it != f5_fuzzy_cache_.end()) {
        hydrate_out_from_cached(it->second);
        return;
    }

    std::vector<std::string_view> grams = build_trigrams(typo);
    grams = unique_sorted_terms(grams);
    if (grams.empty()) {
        return;
    }

    std::vector<const std::vector<std::uint32_t>*> gram_postings;
    gram_postings.reserve(grams.size());
    constexpr std::size_t kMaxGramPostingScan = 200000;
    for (const std::string_view g : grams) {
        const std::vector<std::uint32_t>* ids = f5_trigram_term_ids_.find(g);
        if (ids == nullptr || ids->empty()) {
            continue;
        }
        if (ids->size() <= kMaxGramPostingScan) {
            gram_postings.push_back(ids);
        }
    }
    if (gram_postings.empty()) {
        return;
    }
    std::sort(gram_postings.begin(), gram_postings.end(),
              [](const std::vector<std::uint32_t>* a, const std::vector<std::uint32_t>* b) {
                  return a->size() < b->size();
              });
    constexpr std::size_t kMaxActiveGrams = 4;
    if (gram_postings.size() > kMaxActiveGrams) {
        gram_postings.resize(kMaxActiveGrams);
    }

    std::unordered_map<std::uint32_t, std::uint16_t> overlap_counts;
    overlap_counts.reserve(8192);
    for (const auto* ids : gram_postings) {
        for (const std::uint32_t tid : *ids) {
            ++overlap_counts[tid];
        }
    }
    if (overlap_counts.empty()) {
        return;
    }

    struct FuzzyCandidate {
        std::uint32_t tid = 0;
        std::uint16_t overlap = 0;
        std::uint16_t len_diff = 0;
    };
    std::vector<FuzzyCandidate> candidates;
    candidates.reserve(overlap_counts.size());
    const std::size_t typo_len = typo.size();
    const std::uint16_t min_overlap = static_cast<std::uint16_t>(std::max<std::size_t>(1, grams.size() / 3));
    for (const auto& kv : overlap_counts) {
        if (kv.first >= f5_term_lexicon_.size()) {
            continue;
        }
        const std::string_view term = f5_term_lexicon_[kv.first];
        const std::size_t len_diff =
            typo_len >= term.size() ? (typo_len - term.size()) : (term.size() - typo_len);
        if (len_diff > static_cast<std::size_t>(adaptive_max_edits + 2)) {
            continue;
        }
        if (kv.second < min_overlap) {
            continue;
        }
        candidates.push_back(FuzzyCandidate{
            kv.first, kv.second, static_cast<std::uint16_t>(std::min<std::size_t>(len_diff, 65535u))});
    }
    if (candidates.empty()) {
        return;
    }

    std::sort(candidates.begin(), candidates.end(), [](const FuzzyCandidate& a, const FuzzyCandidate& b) {
        if (a.overlap != b.overlap) {
            return a.overlap > b.overlap;
        }
        if (a.len_diff != b.len_diff) {
            return a.len_diff < b.len_diff;
        }
        return a.tid < b.tid;
    });
    constexpr std::size_t kVerifyCap = 128;
    if (candidates.size() > kVerifyCap) {
        candidates.resize(kVerifyCap);
    }

    std::vector<std::pair<std::uint32_t, float>> cached_hits;
    cached_hits.reserve(24);
    const float max_df_log = std::log1p(static_cast<float>(std::max<std::uint32_t>(1u, f5_max_term_df_)));
    for (const FuzzyCandidate& c : candidates) {
        const std::string_view term = f5_term_lexicon_[c.tid];
        const int dist = bounded_levenshtein_distance(typo, term, adaptive_max_edits);
        if (dist > adaptive_max_edits) {
            continue;
        }
        const float overlap_ratio = static_cast<float>(c.overlap) / static_cast<float>(grams.size());
        const std::uint32_t df = c.tid < f5_term_df_.size() ? f5_term_df_[c.tid] : 1u;
        const float prior_ratio = max_df_log > 0.0f ? (std::log1p(static_cast<float>(df)) / max_df_log) : 0.0f;
        const float prior_factor = 0.90f + 0.20f * std::clamp(prior_ratio, 0.0f, 1.0f);
        const float boost = fuzzy_distance_boost(dist) * (0.80f + 0.20f * overlap_ratio) * prior_factor;
        cached_hits.push_back({c.tid, boost});
    }

    std::sort(cached_hits.begin(), cached_hits.end(),
              [&](const std::pair<std::uint32_t, float>& a, const std::pair<std::uint32_t, float>& b) {
                  if (a.second != b.second) {
                      return a.second > b.second;
                  }
                  const std::string_view ta = a.first < f5_term_lexicon_.size() ? f5_term_lexicon_[a.first] : std::string_view{};
                  const std::string_view tb = b.first < f5_term_lexicon_.size() ? f5_term_lexicon_[b.first] : std::string_view{};
                  return ta < tb;
              });
    constexpr std::size_t kOutCap = 24;
    if (cached_hits.size() > kOutCap) {
        cached_hits.resize(kOutCap);
    }

    if (f5_fuzzy_cache_.find(cache_key) == f5_fuzzy_cache_.end()) {
        while (f5_fuzzy_cache_.size() >= k_f5_fuzzy_cache_cap && !f5_fuzzy_cache_fifo_.empty()) {
            const std::string evict_key = std::move(f5_fuzzy_cache_fifo_.front());
            f5_fuzzy_cache_fifo_.pop_front();
            f5_fuzzy_cache_.erase(evict_key);
        }
        f5_fuzzy_cache_fifo_.push_back(cache_key);
        f5_fuzzy_cache_.emplace(cache_key, cached_hits);
    }

    if (const auto it = f5_fuzzy_cache_.find(cache_key); it != f5_fuzzy_cache_.end()) {
        hydrate_out_from_cached(it->second);
    }
}

namespace {

[[nodiscard]] std::uint32_t posting_list_max_tf(const std::vector<Posting>& postings) noexcept {
    std::uint32_t max_tf = 1u;
    for (const Posting& po : postings) {
        max_tf = std::max(max_tf, po.tf);
    }
    return max_tf;
}

} // namespace

bool ExtremeEngine::build_f5_daat_terms(
    const std::vector<std::pair<std::string_view, float>>& query_terms, const bool require_all_terms,
    std::vector<F5DaatTermState>& terms, F5SearchProfile* profile) const {
    terms.clear();
    terms.reserve(query_terms.size());
    const std::size_t corpus_size = forward_index_.size();

    for (const auto& qt : query_terms) {
        const std::vector<Posting>* plist = keyword_postings(qt.first);
        if (plist == nullptr || plist->empty()) {
            if (require_all_terms) {
                return false;
            }
            continue;
        }
        F5DaatTermState st;
        st.postings = plist;
        const double N = static_cast<double>(corpus_size);
        const double df = static_cast<double>(plist->size());
        const double df_ratio = df / N;
        const float penalty = f5_high_df_penalty(df_ratio) * (is_f5_query_stopword(qt.first) ? 0.35f : 1.0f);
        st.idf = static_cast<float>(std::log((N - df + 0.5) / (df + 0.5) + 1.0)) * penalty * qt.second;
        const std::uint32_t max_tf = posting_list_max_tf(*plist);
        const float K = k_bm25_k1 * (1.0f - k_bm25_b);
        const float tf = static_cast<float>(std::max<std::uint32_t>(1u, max_tf));
        st.max_score = st.idf * (tf * (k_bm25_k1 + 1.0f)) / (tf + K);
        terms.push_back(st);
    }

    if (terms.empty()) {
        return false;
    }
    if (profile != nullptr) {
        profile->matched_query_terms = terms.size();
    }
    return true;
}

[[nodiscard]] bool ExtremeEngine::daat_term_exhausted(const F5DaatTermState& st) const noexcept {
    return st.postings == nullptr || st.pos >= st.postings->size();
}

[[nodiscard]] DocID ExtremeEngine::daat_term_current_doc(const F5DaatTermState& st) const noexcept {
    if (daat_term_exhausted(st)) {
        return static_cast<DocID>(-1);
    }
    return (*st.postings)[st.pos].doc_id;
}

void ExtremeEngine::daat_term_advance(F5DaatTermState& st) const noexcept {
    if (!daat_term_exhausted(st)) {
        ++st.pos;
    }
}

void ExtremeEngine::daat_term_skip_to(F5DaatTermState& st, const DocID target) const noexcept {
    if (st.postings == nullptr) {
        return;
    }
    while (st.pos < st.postings->size() && (*st.postings)[st.pos].doc_id < target) {
        ++st.pos;
    }
}

std::size_t ExtremeEngine::count_or_union_docs(std::vector<F5DaatTermState>& terms) const {
    constexpr DocID kInvalidDocId = static_cast<DocID>(-1);
    std::size_t hits = 0;
    while (true) {
        DocID min_doc = kInvalidDocId;
        for (F5DaatTermState& st : terms) {
            if (daat_term_exhausted(st)) {
                continue;
            }
            const DocID doc = daat_term_current_doc(st);
            if (min_doc == kInvalidDocId || doc < min_doc) {
                min_doc = doc;
            }
        }
        if (min_doc == kInvalidDocId) {
            break;
        }
        ++hits;
        for (F5DaatTermState& st : terms) {
            while (!daat_term_exhausted(st) && daat_term_current_doc(st) == min_doc) {
                daat_term_advance(st);
            }
        }
    }
    for (F5DaatTermState& st : terms) {
        st.pos = 0;
    }
    return hits;
}

void ExtremeEngine::sort_f5_daat_results(std::vector<std::pair<float, DocID>>& ordered) const {
    std::sort(ordered.begin(), ordered.end(), [&](const std::pair<float, DocID>& a, const std::pair<float, DocID>& b) {
        if (a.first != b.first) {
            return a.first > b.first;
        }
        const Document* da = document_at(a.second);
        const Document* db = document_at(b.second);
        const int ya = da != nullptr ? da->year : 0;
        const int yb = db != nullptr ? db->year : 0;
        if (ya != yb) {
            return ya > yb;
        }
        return a.second < b.second;
    });
}

bool ExtremeEngine::daat_wand_or_topk(
    std::vector<F5DaatTermState>& terms, const std::size_t requested_topk,
    std::vector<std::pair<float, DocID>>& ordered, std::size_t& total_hits, F5SearchProfile* profile) const {
    constexpr DocID kInvalidDocId = static_cast<DocID>(-1);
    ordered.clear();
    if (terms.size() == 1 && terms.front().postings != nullptr) {
        total_hits = terms.front().postings->size();
    } else {
        total_hits = count_or_union_docs(terms);
    }
    if (total_hits == 0 || requested_topk == 0) {
        return false;
    }

    std::sort(terms.begin(), terms.end(),
              [](const F5DaatTermState& a, const F5DaatTermState& b) { return a.max_score < b.max_score; });

    using ScoreDoc = std::pair<float, DocID>;
    std::priority_queue<ScoreDoc, std::vector<ScoreDoc>, std::greater<ScoreDoc>> heap;
    float threshold = 0.0f;
    const float avdl = avg_dl_ > 0.0f ? avg_dl_ : 1.0f;

    while (true) {
        std::vector<std::size_t> order;
        order.reserve(terms.size());
        for (std::size_t i = 0; i < terms.size(); ++i) {
            if (!daat_term_exhausted(terms[i])) {
                order.push_back(i);
            }
        }
        if (order.empty()) {
            break;
        }

        std::sort(order.begin(), order.end(), [&](const std::size_t a, const std::size_t b) {
            const DocID da = daat_term_current_doc(terms[a]);
            const DocID db = daat_term_current_doc(terms[b]);
            if (da != db) {
                return da < db;
            }
            return terms[a].max_score > terms[b].max_score;
        });

        float bound = 0.0f;
        std::size_t pivot_order_idx = order.size();
        for (std::size_t oi = 0; oi < order.size(); ++oi) {
            bound += terms[order[oi]].max_score;
            if (bound > threshold) {
                pivot_order_idx = oi;
                break;
            }
        }
        if (pivot_order_idx >= order.size()) {
            if (profile != nullptr) {
                profile->blocks_pruned += 1;
            }
            break;
        }

        const DocID pivot_doc = daat_term_current_doc(terms[order[pivot_order_idx]]);
        if (pivot_doc == kInvalidDocId) {
            break;
        }

        bool pivot_is_candidate = true;
        for (std::size_t oi = 0; oi <= pivot_order_idx; ++oi) {
            if (daat_term_current_doc(terms[order[oi]]) != pivot_doc) {
                pivot_is_candidate = false;
                break;
            }
        }

        if (pivot_is_candidate) {
            float score = 0.0f;
            for (F5DaatTermState& st : terms) {
                if (daat_term_exhausted(st)) {
                    continue;
                }
                daat_term_skip_to(st, pivot_doc);
                if (daat_term_exhausted(st) || daat_term_current_doc(st) != pivot_doc) {
                    continue;
                }
                const Posting& po = (*st.postings)[st.pos];
                const float dl = static_cast<float>(doc_length_for(po.doc_id));
                const float K = k_bm25_k1 * ((1.0f - k_bm25_b) + k_bm25_b * (dl / avdl));
                const float tf = static_cast<float>(po.tf);
                score += st.idf * (tf * (k_bm25_k1 + 1.0f)) / (tf + K);
                if (profile != nullptr) {
                    profile->postings_scored += 1;
                }
            }
            if (profile != nullptr) {
                profile->docs_touched += 1;
            }
            if (heap.size() < requested_topk) {
                heap.push({score, pivot_doc});
                if (heap.size() == requested_topk) {
                    threshold = heap.top().first;
                }
            } else if (score > threshold || (score == threshold && pivot_doc < heap.top().second)) {
                heap.pop();
                heap.push({score, pivot_doc});
                threshold = heap.top().first;
            }
        }

        if (pivot_is_candidate) {
            for (F5DaatTermState& st : terms) {
                if (!daat_term_exhausted(st) && daat_term_current_doc(st) == pivot_doc) {
                    daat_term_advance(st);
                }
            }
        } else {
            for (std::size_t oi = 0; oi < pivot_order_idx; ++oi) {
                daat_term_skip_to(terms[order[oi]], pivot_doc);
            }
        }
    }

    if (heap.empty()) {
        return false;
    }

    ordered.reserve(heap.size());
    while (!heap.empty()) {
        ordered.push_back(heap.top());
        heap.pop();
    }
    sort_f5_daat_results(ordered);
    return true;
}

bool ExtremeEngine::daat_wand_and_topk(
    std::vector<F5DaatTermState>& terms, const std::size_t requested_topk,
    std::vector<std::pair<float, DocID>>& ordered, std::size_t& total_hits, F5SearchProfile* profile) const {
    constexpr DocID kInvalidDocId = static_cast<DocID>(-1);
    ordered.clear();
    total_hits = 0;
    if (requested_topk == 0) {
        return false;
    }

    std::sort(terms.begin(), terms.end(), [](const F5DaatTermState& a, const F5DaatTermState& b) {
        if (a.postings == nullptr) {
            return false;
        }
        if (b.postings == nullptr) {
            return true;
        }
        return a.postings->size() < b.postings->size();
    });

    using ScoreDoc = std::pair<float, DocID>;
    std::priority_queue<ScoreDoc, std::vector<ScoreDoc>, std::greater<ScoreDoc>> heap;
    float threshold = 0.0f;
    const float avdl = avg_dl_ > 0.0f ? avg_dl_ : 1.0f;

    while (true) {
        DocID pivot_doc = kInvalidDocId;
        std::size_t pivot_idx = 0;
        for (std::size_t i = 0; i < terms.size(); ++i) {
            if (!daat_term_exhausted(terms[i])) {
                pivot_doc = daat_term_current_doc(terms[i]);
                pivot_idx = i;
                break;
            }
        }
        if (pivot_doc == kInvalidDocId) {
            break;
        }

        for (F5DaatTermState& st : terms) {
            daat_term_skip_to(st, pivot_doc);
        }

        bool aligned = true;
        for (const F5DaatTermState& st : terms) {
            if (daat_term_exhausted(st) || daat_term_current_doc(st) != pivot_doc) {
                aligned = false;
                break;
            }
        }

        if (aligned) {
            float score = 0.0f;
            for (F5DaatTermState& st : terms) {
                const Posting& po = (*st.postings)[st.pos];
                const float dl = static_cast<float>(doc_length_for(po.doc_id));
                const float K = k_bm25_k1 * ((1.0f - k_bm25_b) + k_bm25_b * (dl / avdl));
                const float tf = static_cast<float>(po.tf);
                score += st.idf * (tf * (k_bm25_k1 + 1.0f)) / (tf + K);
                if (profile != nullptr) {
                    profile->postings_scored += 1;
                }
            }
            ++total_hits;
            if (profile != nullptr) {
                profile->docs_touched += 1;
            }
            if (heap.size() < requested_topk) {
                heap.push({score, pivot_doc});
                if (heap.size() == requested_topk) {
                    threshold = heap.top().first;
                }
            } else if (score > threshold || (score == threshold && pivot_doc < heap.top().second)) {
                heap.pop();
                heap.push({score, pivot_doc});
                threshold = heap.top().first;
            }
        }

        daat_term_advance(terms[pivot_idx]);
    }

    if (heap.empty()) {
        total_hits = 0;
        return false;
    }

    ordered.reserve(heap.size());
    while (!heap.empty()) {
        ordered.push_back(heap.top());
        heap.pop();
    }
    sort_f5_daat_results(ordered);
    return true;
}

bool ExtremeEngine::try_search_f5_daat_wand(
    const std::vector<std::pair<std::string_view, float>>& terms, const bool require_all_terms,
    const std::size_t requested_topk, std::vector<std::pair<float, DocID>>& ordered, std::size_t& total_hits,
    F5SearchProfile* profile) const {
    ordered.clear();
    total_hits = 0;
    if (terms.empty() || requested_topk == 0 || forward_index_.empty()) {
        return false;
    }

    std::vector<F5DaatTermState> daat_terms;
    if (!build_f5_daat_terms(terms, require_all_terms, daat_terms, profile)) {
        return false;
    }

    if (require_all_terms) {
        return daat_wand_and_topk(daat_terms, requested_topk, ordered, total_hits, profile);
    }
    return daat_wand_or_topk(daat_terms, requested_topk, ordered, total_hits, profile);
}

bool ExtremeEngine::try_search_f5_newest_year(
    const std::vector<std::pair<std::string_view, float>>& terms, const bool require_all_terms,
    const std::size_t requested_topk, std::vector<std::pair<float, DocID>>& ordered, std::size_t& total_hits,
    F5SearchProfile* profile) const {
    ordered.clear();
    total_hits = 0;
    if (terms.empty() || requested_topk == 0 || forward_index_.empty() || year_doc_index_.empty()) {
        return false;
    }

    struct NewestTerm {
        std::string_view term;
        const std::vector<Posting>* postings = nullptr;
        float idf = 0.0f;
    };

    const double N = static_cast<double>(forward_index_.size());
    std::vector<NewestTerm> states;
    states.reserve(terms.size());
    std::vector<std::uint16_t> doc_hits(forward_index_.size(), 0u);
    std::vector<DocID> touched_docs;
    touched_docs.reserve(4096);

    for (const auto& q : terms) {
        const std::vector<Posting>* plist = keyword_postings(q.first);
        if (plist == nullptr || plist->empty()) {
            if (require_all_terms) {
                return false;
            }
            continue;
        }
        const double df = static_cast<double>(plist->size());
        const double df_ratio = df / N;
        const float penalty = f5_high_df_penalty(df_ratio) * (is_f5_query_stopword(q.first) ? 0.35f : 1.0f);
        const float idf = static_cast<float>(std::log((N - df + 0.5) / (df + 0.5) + 1.0)) * penalty * q.second;
        states.push_back(NewestTerm{q.first, plist, idf});
        if (profile != nullptr) {
            profile->postings_visited += plist->size();
        }
        for (const Posting& po : *plist) {
            if (po.doc_id >= doc_hits.size()) {
                continue;
            }
            if (doc_hits[po.doc_id] == 0u) {
                touched_docs.push_back(po.doc_id);
            }
            ++doc_hits[po.doc_id];
        }
    }

    if (states.empty()) {
        return false;
    }
    if (profile != nullptr) {
        profile->matched_query_terms = states.size();
    }

    for (const DocID did : touched_docs) {
        if (!require_all_terms || doc_hits[did] == states.size()) {
            ++total_hits;
        }
    }
    if (total_hits == 0) {
        return false;
    }

    std::vector<int> years;
    years.reserve(year_doc_index_.size());
    for (const auto& entry : year_doc_index_) {
        years.push_back(entry.first);
    }
    std::sort(years.begin(), years.end(), std::greater<int>());

    const float avdl = avg_dl_ > 0.0f ? avg_dl_ : 1.0f;
    for (const int year : years) {
        const auto it = year_doc_index_.find(year);
        if (it == year_doc_index_.end()) {
            continue;
        }
        for (const DocID did : it->second) {
            if (did >= doc_hits.size() || doc_hits[did] == 0u) {
                continue;
            }
            if (require_all_terms && doc_hits[did] != states.size()) {
                continue;
            }
            float score = 0.0f;
            for (const NewestTerm& st : states) {
                const auto pos = std::lower_bound(
                    st.postings->begin(), st.postings->end(), did,
                    [](const Posting& po, const DocID target) { return po.doc_id < target; });
                if (pos == st.postings->end() || pos->doc_id != did) {
                    continue;
                }
                const float dl = static_cast<float>(doc_length_for(did));
                const float K = k_bm25_k1 * ((1.0f - k_bm25_b) + k_bm25_b * (dl / avdl));
                const float tf = static_cast<float>(pos->tf);
                score += st.idf * (tf * (k_bm25_k1 + 1.0f)) / (tf + K);
                if (profile != nullptr) {
                    profile->postings_scored += 1;
                }
            }
            ordered.push_back({score, did});
            if (profile != nullptr) {
                profile->docs_touched += 1;
            }
            if (ordered.size() >= requested_topk) {
                goto newest_done;
            }
        }
    }

newest_done:
    if (ordered.empty()) {
        return false;
    }
    std::sort(ordered.begin(), ordered.end(), [&](const std::pair<float, DocID>& a, const std::pair<float, DocID>& b) {
        const int ya = a.second < forward_index_.size() ? forward_index_[a.second].year : 0;
        const int yb = b.second < forward_index_.size() ? forward_index_[b.second].year : 0;
        if (ya != yb) {
            return ya > yb;
        }
        if (a.first != b.first) {
            return a.first > b.first;
        }
        return a.second < b.second;
    });
    return true;
}

bool ExtremeEngine::try_search_f5_prefix_topk(
    const std::vector<std::pair<std::string_view, float>>& terms, const std::size_t requested_topk,
    std::vector<std::pair<float, DocID>>& ordered, std::size_t& total_hits, F5SearchProfile* profile) const {
    ordered.clear();
    total_hits = 0;
    if (terms.empty() || requested_topk == 0 || forward_index_.empty()) {
        return false;
    }

    if (scoring_board_.size() != forward_index_.size()) {
        scoring_board_.assign(forward_index_.size(), 0.0f);
    }

    const double N = static_cast<double>(forward_index_.size());
    const float avdl = avg_dl_ > 0.0f ? avg_dl_ : 1.0f;
    std::vector<DocID> modified_docs;
    modified_docs.reserve(4096);
    std::size_t matched_terms = 0;

    for (const auto& q : terms) {
        const std::vector<Posting>* plist = keyword_postings(q.first);
        if (plist == nullptr || plist->empty()) {
            continue;
        }
        ++matched_terms;
        if (profile != nullptr) {
            profile->postings_visited += plist->size();
        }
        const double df = static_cast<double>(plist->size());
        const double df_ratio = df / N;
        const float penalty = f5_high_df_penalty(df_ratio) * (is_f5_query_stopword(q.first) ? 0.35f : 1.0f);
        const float idf = static_cast<float>(std::log((N - df + 0.5) / (df + 0.5) + 1.0)) * penalty * q.second;
        for (const Posting& po : *plist) {
            const DocID did = po.doc_id;
            if (did >= forward_index_.size()) {
                continue;
            }
            const float dl = static_cast<float>(doc_length_for(did));
            const float K = k_bm25_k1 * ((1.0f - k_bm25_b) + k_bm25_b * (dl / avdl));
            const float tf = static_cast<float>(po.tf);
            const float inc = idf * (tf * (k_bm25_k1 + 1.0f)) / (tf + K);
            const float prev = scoring_board_[did];
            scoring_board_[did] = prev + inc;
            if (prev == 0.0f) {
                modified_docs.push_back(did);
            }
            if (profile != nullptr) {
                profile->postings_scored += 1;
            }
        }
    }

    if (matched_terms == 0 || modified_docs.empty()) {
        return false;
    }
    total_hits = modified_docs.size();
    ordered.reserve(modified_docs.size());
    for (const DocID did : modified_docs) {
        ordered.push_back({scoring_board_[did], did});
    }

    auto better = [&](const std::pair<float, DocID>& a, const std::pair<float, DocID>& b) {
        if (a.first != b.first) {
            return a.first > b.first;
        }
        const int ya = a.second < forward_index_.size() ? forward_index_[a.second].year : 0;
        const int yb = b.second < forward_index_.size() ? forward_index_[b.second].year : 0;
        if (ya != yb) {
            return ya > yb;
        }
        return a.second < b.second;
    };
    if (ordered.size() > requested_topk) {
        std::nth_element(ordered.begin(), ordered.begin() + static_cast<std::ptrdiff_t>(requested_topk),
                         ordered.end(), better);
        ordered.resize(requested_topk);
    }
    std::sort(ordered.begin(), ordered.end(), better);

    for (const DocID did : modified_docs) {
        scoring_board_[did] = 0.0f;
    }
    if (profile != nullptr) {
        profile->matched_query_terms = matched_terms;
        profile->docs_touched += modified_docs.size();
        profile->total_hits = total_hits;
    }
    return !ordered.empty();
}

bool ExtremeEngine::try_search_f5_single_term_blockmax(
    std::string_view term, float query_boost, std::size_t requested_topk,
    std::vector<std::pair<float, DocID>>& ordered, std::size_t& total_hits,
    F5SearchProfile* profile) const {
    ordered.clear();
    total_hits = 0;
    if (requested_topk == 0 || forward_index_.empty()) {
        return false;
    }
    const std::vector<Posting>* plist = keyword_global_.find(term);
    if (plist == nullptr || plist->empty()) {
        return false;
    }
    const std::vector<F5PostingBlockMeta>* blocks = f5_term_block_meta_.find(term);
    if (blocks == nullptr || blocks->empty()) {
        return false;
    }

    total_hits = plist->size();
    if (profile != nullptr) {
        profile->postings_visited += plist->size();
        profile->matched_query_terms = 1;
    }
    const double N = static_cast<double>(forward_index_.size());
    const double df = static_cast<double>(plist->size());
    const double df_ratio = df / N;
    const float penalty = f5_high_df_penalty(df_ratio) * (is_f5_query_stopword(term) ? 0.35f : 1.0f);
    const float idf = static_cast<float>(std::log((N - df + 0.5) / (df + 0.5) + 1.0)) * penalty * query_boost;
    const float avdl = avg_dl_ > 0.0f ? avg_dl_ : 1.0f;

    auto score_posting = [&](const Posting& po) {
        const float dl = static_cast<float>(std::max<std::uint32_t>(1u, forward_index_[po.doc_id].doc_length));
        const float K = k_bm25_k1 * ((1.0f - k_bm25_b) + k_bm25_b * (dl / avdl));
        const float tf = static_cast<float>(po.tf);
        return idf * (tf * (k_bm25_k1 + 1.0f)) / (tf + K);
    };
    auto max_block_score = [&](const F5PostingBlockMeta& block) {
        const float tf = static_cast<float>(std::max<std::uint16_t>(1u, block.max_tf));
        const float K = k_bm25_k1 * (1.0f - k_bm25_b);
        return idf * (tf * (k_bm25_k1 + 1.0f)) / (tf + K);
    };

    using ScoreDoc = std::pair<float, DocID>;
    std::priority_queue<ScoreDoc, std::vector<ScoreDoc>, std::greater<ScoreDoc>> heap;
    float threshold = 0.0f;
    constexpr std::size_t kBlockSize = 128;
    for (std::size_t bi = 0; bi < blocks->size(); ++bi) {
        if (heap.size() >= requested_topk && max_block_score((*blocks)[bi]) <= threshold) {
            if (profile != nullptr) {
                profile->blocks_pruned += 1;
            }
            continue;
        }
        const std::size_t beg = bi * kBlockSize;
        const std::size_t end = std::min<std::size_t>(plist->size(), beg + kBlockSize);
        for (std::size_t i = beg; i < end; ++i) {
            const Posting& po = (*plist)[i];
            if (po.doc_id >= forward_index_.size()) {
                continue;
            }
            if (profile != nullptr) {
                profile->postings_scored += 1;
            }
            const float score = score_posting(po);
            if (heap.size() < requested_topk) {
                heap.push({score, po.doc_id});
                if (heap.size() == requested_topk) {
                    threshold = heap.top().first;
                }
            } else if (score > threshold || (score == threshold && po.doc_id < heap.top().second)) {
                heap.pop();
                heap.push({score, po.doc_id});
                threshold = heap.top().first;
            }
        }
    }

    ordered.reserve(heap.size());
    while (!heap.empty()) {
        ordered.push_back(heap.top());
        heap.pop();
    }
    std::sort(ordered.begin(), ordered.end(), [&](const ScoreDoc& a, const ScoreDoc& b) {
        if (a.first != b.first) {
            return a.first > b.first;
        }
        const int ya = forward_index_[a.second].year;
        const int yb = forward_index_[b.second].year;
        if (ya != yb) {
            return ya > yb;
        }
        return a.second < b.second;
    });
    if (profile != nullptr) {
        profile->docs_touched = ordered.size();
        profile->total_hits = total_hits;
    }
    return !ordered.empty();
}

bool ExtremeEngine::try_search_f5_or_blockmax(
    const std::vector<std::pair<std::string_view, float>>& terms, std::size_t requested_topk,
    std::vector<std::pair<float, DocID>>& ordered, std::size_t& total_hits,
    F5SearchProfile* profile) const {
    ordered.clear();
    total_hits = 0;
    if (terms.size() < 2 || requested_topk == 0 || forward_index_.empty()) {
        return false;
    }

    struct TermState {
        const std::vector<Posting>* postings = nullptr;
        const std::vector<F5PostingBlockMeta>* blocks = nullptr;
        float idf = 0.0f;
    };

    const double N = static_cast<double>(forward_index_.size());
    std::vector<TermState> states;
    states.reserve(terms.size());
    std::uint64_t total_postings = 0;
    for (const auto& q : terms) {
        const std::vector<Posting>* plist = keyword_global_.find(q.first);
        const std::vector<F5PostingBlockMeta>* blocks = f5_term_block_meta_.find(q.first);
        if (plist == nullptr || plist->empty() || blocks == nullptr || blocks->empty()) {
            return false;
        }
        const double df = static_cast<double>(plist->size());
        const double df_ratio = df / N;
        const float penalty = f5_high_df_penalty(df_ratio) * (is_f5_query_stopword(q.first) ? 0.35f : 1.0f);
        const float idf = static_cast<float>(std::log((N - df + 0.5) / (df + 0.5) + 1.0)) * penalty * q.second;
        states.push_back(TermState{plist, blocks, idf});
        total_postings += plist->size();
        if (profile != nullptr) {
            profile->postings_visited += plist->size();
        }
    }
    if (profile != nullptr) {
        profile->matched_query_terms = terms.size();
    }

    std::unordered_map<DocID, float> scores;
    scores.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(total_postings, 1u << 20)));
    const float avdl = avg_dl_ > 0.0f ? avg_dl_ : 1.0f;
    constexpr std::size_t kBlockSize = 128;

    for (const TermState& st : states) {
        auto max_block_score = [&](const F5PostingBlockMeta& block) {
            const float tf = static_cast<float>(std::max<std::uint16_t>(1u, block.max_tf));
            const float K = k_bm25_k1 * (1.0f - k_bm25_b);
            return st.idf * (tf * (k_bm25_k1 + 1.0f)) / (tf + K);
        };

        std::vector<std::pair<float, std::size_t>> block_order;
        block_order.reserve(st.blocks->size());
        for (std::size_t bi = 0; bi < st.blocks->size(); ++bi) {
            block_order.push_back({max_block_score((*st.blocks)[bi]), bi});
        }
        std::sort(block_order.begin(), block_order.end(),
                  [](const auto& a, const auto& b) {
                      if (a.first != b.first) {
                          return a.first > b.first;
                      }
                      return a.second < b.second;
                  });

        std::priority_queue<std::pair<float, DocID>, std::vector<std::pair<float, DocID>>,
                            std::greater<std::pair<float, DocID>>>
            heap_probe;
        float threshold = 0.0f;
        for (const auto& bm : block_order) {
            if (heap_probe.size() >= requested_topk && bm.first <= threshold) {
                if (profile != nullptr) {
                    profile->blocks_pruned += 1;
                }
                break;
            }
            const std::size_t bi = bm.second;
            const std::size_t beg = bi * kBlockSize;
            const std::size_t end = std::min<std::size_t>(st.postings->size(), beg + kBlockSize);
            for (std::size_t i = beg; i < end; ++i) {
                const Posting& po = (*st.postings)[i];
                if (po.doc_id >= forward_index_.size()) {
                    continue;
                }
                if (profile != nullptr) {
                    profile->postings_scored += 1;
                }
                const float dl = static_cast<float>(std::max<std::uint32_t>(1u, forward_index_[po.doc_id].doc_length));
                const float K = k_bm25_k1 * ((1.0f - k_bm25_b) + k_bm25_b * (dl / avdl));
                const float tf = static_cast<float>(po.tf);
                const float inc = st.idf * (tf * (k_bm25_k1 + 1.0f)) / (tf + K);
                scores[po.doc_id] += inc;

                const float s = scores[po.doc_id];
                if (heap_probe.size() < requested_topk) {
                    heap_probe.push({s, po.doc_id});
                    if (heap_probe.size() == requested_topk) {
                        threshold = heap_probe.top().first;
                    }
                } else if (s > threshold || (s == threshold && po.doc_id < heap_probe.top().second)) {
                    heap_probe.pop();
                    heap_probe.push({s, po.doc_id});
                    threshold = heap_probe.top().first;
                }
            }
        }
    }

    total_hits = scores.size();
    if (scores.empty()) {
        return false;
    }

    using ScoreDoc = std::pair<float, DocID>;
    std::priority_queue<ScoreDoc, std::vector<ScoreDoc>, std::greater<ScoreDoc>> top;
    for (const auto& kv : scores) {
        const ScoreDoc sd{kv.second, kv.first};
        if (top.size() < requested_topk) {
            top.push(sd);
        } else if (sd.first > top.top().first || (sd.first == top.top().first && sd.second < top.top().second)) {
            top.pop();
            top.push(sd);
        }
    }

    ordered.reserve(top.size());
    while (!top.empty()) {
        ordered.push_back(top.top());
        top.pop();
    }
    std::sort(ordered.begin(), ordered.end(), [&](const ScoreDoc& a, const ScoreDoc& b) {
        if (a.first != b.first) {
            return a.first > b.first;
        }
        const int ya = forward_index_[a.second].year;
        const int yb = forward_index_[b.second].year;
        if (ya != yb) {
            return ya > yb;
        }
        return a.second < b.second;
    });
    if (profile != nullptr) {
        profile->docs_touched = scores.size();
        profile->total_hits = total_hits;
    }
    return !ordered.empty();
}

void ExtremeEngine::rebuild_f3_top100_cache() {
    using AuthorCount = std::pair<std::size_t, std::string_view>;
    using MinHeap = std::priority_queue<AuthorCount, std::vector<AuthorCount>, std::greater<AuthorCount>>;

    MinHeap top100;
    author_global_.for_each([&](std::string_view author, const std::vector<Posting>& postings) {
        const std::size_t paper_count = postings.size();
        if (paper_count == 0) {
            return;
        }
        if (top100.size() < 100) {
            top100.push({paper_count, author});
        } else if (paper_count > top100.top().first) {
            top100.pop();
            top100.push({paper_count, author});
        }
    });

    f3_top100_cache_.clear();
    f3_top100_cache_.reserve(top100.size());
    while (!top100.empty()) {
        f3_top100_cache_.push_back(top100.top());
        top100.pop();
    }
    std::reverse(f3_top100_cache_.begin(), f3_top100_cache_.end());
}

void ExtremeEngine::rebuild_year_doc_index() {
    year_doc_index_.clear();
    year_doc_index_.reserve(256);
    for (const Document& doc : forward_index_) {
        if (doc.title.empty()) {
            continue;
        }
        year_doc_index_[doc.year].push_back(doc.id);
    }
}

void ExtremeEngine::rebuild_f4_year_term_cache() {
    f4_year_term_cache_.clear();
    f4_year_term_cache_.reserve(std::max<std::size_t>(64, year_doc_index_.size() * 2));
    f4_term_intern_.clear();
    f4_term_intern_.reserve(std::max<std::size_t>(4096, keyword_global_.size() / 2));
    f4_term_arena_.reset();

    auto intern_f4_term = [&](std::string_view token) -> std::string_view {
        const auto it = f4_term_intern_.find(token);
        if (it != f4_term_intern_.end()) {
            return it->second;
        }
        const std::string_view stable = f4_term_arena_.store(token);
        f4_term_intern_.emplace(stable, stable);
        return stable;
    };

    StringArena token_arena(4u * 1024u * 1024u);
    std::vector<std::string_view> filtered_tokens;
    filtered_tokens.reserve(64);
    std::size_t processed_docs = 0;

    for (const Document& doc : forward_index_) {
        if (doc.title.empty()) {
            continue;
        }
        const std::vector<std::string_view> raw_tokens = Analyzer::normalize_and_tokenize(doc.title, token_arena);
        if (raw_tokens.empty()) {
            continue;
        }
        build_f4_canonical_filtered_tokens(raw_tokens, filtered_tokens);
        if (!filtered_tokens.empty()) {
            auto& year_terms = f4_year_term_cache_[doc.year];
            if (year_terms.empty()) {
                year_terms.reserve(2048);
            }
            for (const std::string_view token : filtered_tokens) {
                const std::string_view stable_term = intern_f4_term(token);
                auto [it, inserted] = year_terms.emplace(stable_term, 0);
                (void)inserted;
                ++it->second;
            }
        }

        ++processed_docs;
        if ((processed_docs & 1023u) == 0u) {
            token_arena.reset();
        }
    }
}

std::unordered_map<std::string, int> ExtremeEngine::compute_f4_term_counts_for_year(int year) const {
    const auto it = f4_year_term_cache_.find(year);
    if (it == f4_year_term_cache_.end() || it->second.empty()) {
        return {};
    }

    std::unordered_map<std::string, int> out;
    out.reserve(it->second.size());
    for (const auto& kv : it->second) {
        out.emplace(std::string(kv.first), kv.second);
    }
    return out;
}

std::vector<ExtremeEngine::F4HotEntry> ExtremeEngine::compute_f4_top10_for_year(int year) const {
    const auto cache_it = f4_year_term_cache_.find(year);
    if (cache_it == f4_year_term_cache_.end() || cache_it->second.empty()) {
        return {};
    }

    std::vector<std::pair<std::string_view, int>> ranked;
    ranked.reserve(cache_it->second.size());
    for (const auto& kv : cache_it->second) {
        ranked.push_back({kv.first, kv.second});
    }

    const auto better = [](const std::pair<std::string_view, int>& a, const std::pair<std::string_view, int>& b) {
        if (a.second != b.second) {
            return a.second > b.second;
        }
        return a.first < b.first;
    };

    constexpr std::size_t kTopN = 10;
    if (ranked.size() > kTopN) {
        std::nth_element(ranked.begin(), ranked.begin() + static_cast<std::ptrdiff_t>(kTopN), ranked.end(),
                         [&](const std::pair<std::string_view, int>& lhs, const std::pair<std::string_view, int>& rhs) {
                             return better(lhs, rhs);
                         });
        ranked.resize(kTopN);
    }
    std::sort(ranked.begin(), ranked.end(), better);

    std::vector<F4HotEntry> out;
    out.reserve(ranked.size());
    for (const auto& kv : ranked) {
        out.push_back(F4HotEntry{std::string(kv.first), kv.second, 0, 0.0, static_cast<double>(kv.second)});
    }
    return out;
}

std::string_view ExtremeEngine::build_normalized_lookup_key(std::string_view raw_query) const {
    query_arena_.reset();
    const std::vector<std::string_view> toks = Analyzer::normalize_and_tokenize(raw_query, query_arena_);
    if (toks.empty()) {
        return {};
    }
    const char* const beg = toks.front().data();
    const char* const ed = toks.back().data() + toks.back().size();
    return {beg, static_cast<std::size_t>(ed - beg)};
}

void ExtremeEngine::print_document_details(const Document& doc, std::ostream& os) const {
    os << "[DocID] " << doc.id << '\n';
    if (!doc.title.empty()) {
        os << "[Title] " << doc.title << '\n';
    }
    if (!doc.authors.empty()) {
        os << "[Authors] " << doc.authors << '\n';
    }
    if (!doc.journal.empty()) {
        os << "[Journal] " << doc.journal << '\n';
    }
    os << "[Year] " << doc.year << '\n';
    os << "[DocLength] " << doc.doc_length << '\n';
    if (!doc.volume.empty()) {
        os << "[Volume] " << doc.volume << '\n';
    }
    if (!doc.month.empty()) {
        os << "[Month] " << doc.month << '\n';
    }
    if (!doc.cdrom.empty()) {
        os << "[CDROM] " << doc.cdrom << '\n';
    }
    if (!doc.ee.empty()) {
        os << "[EE] " << doc.ee << '\n';
    }
    if (!doc.url.empty()) {
        os << "[URL] " << doc.url << '\n';
    }
}

void ExtremeEngine::search_by_author(std::string_view query, std::ostream& os) const {
    const F1AuthorQueryPlan plan = parse_f1_author_query_plan(query);
    const std::string_view effective_query = plan.author_query.empty() ? query : std::string_view(plan.author_query);
    const std::string_view key = build_normalized_lookup_key(effective_query);
    if (key.empty()) {
        os << "查询无效（无法规范化）\n";
        return;
    }

    auto emit_postings = [&](std::string_view matched_author, const std::vector<Posting>& hits) {
        os << "[Author] " << matched_author << " | [Papers] " << hits.size() << "\n\n";
        for (const Posting& po : hits) {
            if (po.doc_id >= forward_index_.size()) {
                continue;
            }
            print_document_details(forward_index_[po.doc_id], os);
            os << '\n';
        }
    };

    if (const std::vector<Posting>* plist = author_global_.find(key); plist != nullptr && !plist->empty()) {
        emit_postings(key, *plist);
        return;
    }

    if (!plan.fuzzy_enabled || key.size() < 3) {
        os << "未找到匹配作者。\n";
        return;
    }

    std::vector<std::pair<std::string_view, float>> fuzzy_authors;
    collect_f1_fuzzy_author_candidates(key, plan.fuzzy_max_edits, fuzzy_authors);
    if (fuzzy_authors.empty()) {
        os << "未找到匹配作者。\n";
        return;
    }

    const std::string_view best_author = fuzzy_authors.front().first;
    const std::vector<Posting>* plist = author_global_.find(best_author);
    if (plist == nullptr || plist->empty()) {
        os << "未找到匹配作者。\n";
        return;
    }

    os << "[F1-Fuzzy] " << key << " -> " << best_author << " | [Score] " << fuzzy_authors.front().second << "\n";
    if (fuzzy_authors.size() > 1) {
        os << "[候选作者Top3] ";
        const std::size_t shown = std::min<std::size_t>(3, fuzzy_authors.size());
        for (std::size_t i = 0; i < shown; ++i) {
            if (i != 0) {
                os << " ; ";
            }
            os << fuzzy_authors[i].first;
        }
        os << "\n\n";
    } else {
        os << "\n";
    }

    for (const Posting& po : *plist) {
        if (po.doc_id >= forward_index_.size()) {
            continue;
        }
        print_document_details(forward_index_[po.doc_id], os);
        os << '\n';
    }
}

void ExtremeEngine::search_by_title(std::string_view query, std::ostream& os) const {
    const std::string_view key = build_normalized_lookup_key(query);
    if (key.empty()) {
        os << "查询无效（无法规范化）\n";
        return;
    }
    const std::vector<Posting>* plist = title_exact_global_.find(key);
    auto emit_postings = [&](const std::vector<Posting>& hits) {
        for (const Posting& po : hits) {
            if (po.doc_id >= forward_index_.size()) {
                continue;
            }
            print_document_details(forward_index_[po.doc_id], os);
            os << '\n';
        }
    };
    if (plist != nullptr && !plist->empty()) {
        emit_postings(*plist);
        return;
    }

    // Fallback path:
    // When normalized title exact index misses due normalization-edge mismatch, use
    // rarest query term postings as candidates, then verify full normalized title equality.
    const std::string key_copy(key);
    std::vector<std::string_view> q_terms;
    q_terms.reserve(16);
    {
        std::size_t i = 0;
        while (i < key_copy.size()) {
            while (i < key_copy.size() && key_copy[i] == ' ') {
                ++i;
            }
            if (i >= key_copy.size()) {
                break;
            }
            const std::size_t beg = i;
            while (i < key_copy.size() && key_copy[i] != ' ') {
                ++i;
            }
            q_terms.emplace_back(key_copy.data() + beg, i - beg);
        }
    }
    if (q_terms.empty()) {
        os << "未找到精确匹配标题。\n";
        return;
    }

    const std::vector<Posting>* seed = nullptr;
    for (const std::string_view t : q_terms) {
        const std::vector<Posting>* cur = keyword_global_.find(t);
        if (cur == nullptr || cur->empty()) {
            continue;
        }
        if (seed == nullptr || cur->size() < seed->size()) {
            seed = cur;
        }
    }
    if (seed == nullptr || seed->empty()) {
        os << "未找到精确匹配标题。\n";
        return;
    }

    StringArena verify_arena(256u * 1024u);
    std::vector<Posting> recovered;
    recovered.reserve(4);
    std::size_t checked = 0;

    for (const Posting& po : *seed) {
        if (po.doc_id >= forward_index_.size()) {
            continue;
        }
        const Document& doc = forward_index_[po.doc_id];
        if (doc.title.empty()) {
            continue;
        }
        const std::string_view normalized_doc_title = normalized_span(verify_arena, doc.title);
        if (!normalized_doc_title.empty() && normalized_doc_title == key_copy) {
            recovered.push_back(Posting{po.doc_id, 1u});
        }
        ++checked;
        if ((checked & 2047u) == 0u) {
            verify_arena.reset();
        }
    }

    if (recovered.empty()) {
        os << "未找到精确匹配标题。\n";
        return;
    }
    emit_postings(recovered);
}

void ExtremeEngine::search_collaborators(std::string_view target_author, std::ostream& os) const {
    query_arena_.reset();
    const std::vector<std::string_view> key_toks = Analyzer::normalize_and_tokenize(target_author, query_arena_);
    if (key_toks.empty()) {
        os << "查询无效（无法规范化）\n";
        return;
    }
    const char* const kb = key_toks.front().data();
    const char* const ke = key_toks.back().data() + key_toks.back().size();
    const std::string_view key_span(kb, static_cast<std::size_t>(ke - kb));

    const std::vector<Posting>* plist = author_global_.find(key_span);
    if (plist == nullptr || plist->empty()) {
        os << "未找到该作者。\n";
        return;
    }

    FlatMap<std::string_view, std::uint8_t> dedupe;

    for (const Posting& po : *plist) {
        if (po.doc_id >= forward_index_.size()) {
            continue;
        }
        const Document& doc = forward_index_[po.doc_id];
        if (doc.authors.empty()) {
            continue;
        }
        for_each_author_segment(doc.authors, [&](std::string_view seg) {
            const std::string_view trimmed = trim_sv(seg);
            if (trimmed.empty()) {
                return;
            }
            const std::vector<std::string_view> co_toks = Analyzer::normalize_and_tokenize(trimmed, query_arena_);
            if (co_toks.empty()) {
                return;
            }
            const char* const cb = co_toks.front().data();
            const char* const ce = co_toks.back().data() + co_toks.back().size();
            const std::string_view nk(cb, static_cast<std::size_t>(ce - cb));
            if (nk == key_span) {
                return;
            }
            if (dedupe.find(nk) != nullptr) {
                return;
            }
            dedupe[nk] = 1;
            os << trimmed << '\n';
        });
    }
}

void ExtremeEngine::search_bm25(std::string_view keywords, std::ostream& os, F5SearchOptions opts) const {
    const auto t_wall0 = std::chrono::steady_clock::now();
    F5SearchProfile* profile = opts.profile;
    active_profile_ = profile;
    if (profile != nullptr) {
        profile->reset();
    }

    const std::size_t n = forward_index_.size();
    if (n == 0) {
        os << "索引为空。\n";
        if (profile != nullptr) {
            profile->path = F5ExecPath::InvalidQuery;
            profile->total_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_wall0).count();
        }
        active_profile_ = nullptr;
        return;
    }
    if (scoring_board_.size() != n) {
        scoring_board_.assign(n, 0.0f);
    }

    const auto t_parse0 = std::chrono::steady_clock::now();
    query_arena_.reset();
    const F5QueryPlan plan = parse_f5_query_plan(keywords);
    const double parse_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_parse0).count();
    if (profile != nullptr) {
        profile->parse_ms = parse_ms;
    }
    using ScoreDoc = std::pair<float, DocID>;
    struct F5WindowView {
        bool page_mode = false;
        std::size_t start = 0;
        std::size_t end = 0;
        std::size_t current_page = 1;
        std::size_t page_size = 0;
        std::size_t total_pages = 1;
    };
    const auto compute_window = [&](std::size_t total_hits) -> F5WindowView {
        F5WindowView w;
        w.page_mode = (plan.page > 0 || plan.size > 0);
        if (total_hits == 0) {
            return w;
        }
        if (w.page_mode) {
            w.page_size = plan.size > 0 ? plan.size : (plan.limit > 0 ? plan.limit : 20);
            w.current_page = plan.page > 0 ? plan.page : 1;
            w.total_pages = (total_hits + w.page_size - 1) / w.page_size;
            if (w.current_page > w.total_pages) {
                w.current_page = w.total_pages;
            }
            w.start = (w.current_page - 1) * w.page_size;
            w.end = std::min<std::size_t>(total_hits, w.start + w.page_size);
        } else {
            w.start = std::min<std::size_t>(plan.offset, total_hits);
            w.end = total_hits;
            if (plan.limit > 0) {
                w.end = std::min<std::size_t>(total_hits, w.start + plan.limit);
            }
        }
        return w;
    };
    const std::string cache_key = std::to_string(n) + "|" + std::string(keywords);
    auto emit_ranked_results = [&](const std::vector<ScoreDoc>& ordered_docs, std::size_t total_hits,
                                   std::size_t matched_query_terms, std::size_t fuzzy_rewrite_count,
                                   bool cache_hit, F5ExecPath path_tag) {
        const auto t_emit0 = std::chrono::steady_clock::now();
        const F5WindowView w = compute_window(total_hits);
        if (profile != nullptr) {
            profile->path = path_tag;
            profile->result_cache_hit = cache_hit;
            profile->total_hits = total_hits;
            profile->matched_query_terms = matched_query_terms;
            profile->fuzzy_rewrite_count = fuzzy_rewrite_count;
            profile->requested_topk = w.page_mode ? w.current_page * w.page_size : plan.limit;
            if (profile->requested_topk == 0 && w.page_mode) {
                profile->requested_topk = w.page_size;
            }
        }
        if (!opts.emit_results) {
            if (profile != nullptr) {
                profile->results_emitted = (w.end > w.start) ? (w.end - w.start) : 0;
                profile->emit_ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_emit0).count();
            }
            return;
        }
        os << "[Mode] " << (plan.boolean_mode == F5BooleanMode::And ? "AND" : "OR")
           << " | [Sort] " << (plan.sort_mode == F5SortMode::Newest ? "newest" : "relevance")
           << " | [Fuzzy] " << (plan.fuzzy_enabled ? "on" : "off")
           << '/' << plan.fuzzy_max_edits << '/' << plan.fuzzy_max_expansions
           << " | [Matched Terms] " << matched_query_terms
           << " | [FuzzyRewrite] " << fuzzy_rewrite_count
           << " | [TotalHits] " << total_hits;
        if (cache_hit) {
            os << " | [ResultCache] hit";
        }
        if (w.page_mode) {
            os << " | [Page] " << w.current_page << "/" << w.total_pages
               << " | [PageSize] " << w.page_size
               << " | [Window] " << w.start << "-" << (w.end == 0 ? 0 : w.end - 1);
            if (plan.offset > 0) {
                os << "\n[Hint] page/size 模式下 offset 已忽略。\n";
            }
            if (w.current_page < w.total_pages) {
                os << "\n[Hint] 下一页可使用: page:" << (w.current_page + 1) << " size:" << w.page_size << "\n";
            }
        } else {
            os << " | [Window] " << w.start << "-" << (w.end == 0 ? 0 : w.end - 1)
               << " | [Limit] " << (plan.limit == 0 ? "ALL" : std::to_string(plan.limit));
        }
        os << "\n\n";

        for (std::size_t i = w.start; i < w.end; ++i) {
            const ScoreDoc& e = ordered_docs[i];
            print_document_details(forward_index_[e.second], os);
            os << "[Rank] " << (i + 1) << '\n';
            os << "[BM25] " << e.first << "\n\n";
        }
        if (profile != nullptr) {
            profile->results_emitted = (w.end > w.start) ? (w.end - w.start) : 0;
            profile->emit_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_emit0).count();
        }
    };
    if (const auto it = f5_result_cache_.find(cache_key); it != f5_result_cache_.end()) {
        const F5ResultCacheEntry& cached = it->second;
        const F5WindowView w = compute_window(cached.total_hits);
        if (w.end <= cached.ordered_prefix.size()) {
            emit_ranked_results(cached.ordered_prefix, cached.total_hits, cached.matched_query_terms,
                                cached.fuzzy_rewrite_count, true, F5ExecPath::ResultCache);
            if (profile != nullptr) {
                profile->total_ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_wall0).count();
            }
            active_profile_ = nullptr;
            return;
        }
    }

    const auto t_rewrite0 = std::chrono::steady_clock::now();

    auto get_keyword_postings = [&](std::string_view term) -> const std::vector<Posting>* {
        std::string key(term);
        if (const auto it = f5_hot_term_postings_cache_.find(key); it != f5_hot_term_postings_cache_.end()) {
            return it->second;
        }
        const std::vector<Posting>* plist = keyword_global_.find(term);
        if (plist == nullptr || plist->empty()) {
            return plist;
        }
        while (f5_hot_term_postings_cache_.size() >= k_f5_hot_term_postings_cache_cap &&
               !f5_hot_term_postings_cache_fifo_.empty()) {
            const std::string evict = std::move(f5_hot_term_postings_cache_fifo_.front());
            f5_hot_term_postings_cache_fifo_.pop_front();
            f5_hot_term_postings_cache_.erase(evict);
        }
        auto [insert_it, inserted] = f5_hot_term_postings_cache_.emplace(std::move(key), plist);
        if (inserted) {
            f5_hot_term_postings_cache_fifo_.push_back(insert_it->first);
        }
        return plist;
    };

    const std::string_view term_query = std::string_view(plan.terms_query);
    std::vector<std::string_view> dedup_terms;
    std::vector<std::string_view> raw_terms = Analyzer::normalize_and_tokenize(term_query, query_arena_);
    dedup_terms.reserve(raw_terms.size() + 64);
    std::unordered_map<std::string_view, std::uint8_t> term_seen;
    term_seen.reserve(raw_terms.size() * 4 + 64);
    for (const std::string_view t : raw_terms) {
        const auto [it, inserted] = term_seen.emplace(t, 1u);
        if (inserted) {
            dedup_terms.push_back(it->first);
        }
    }

    std::vector<std::string_view> expanded_terms;
    for (const std::string& pfx : plan.prefix_terms) {
        collect_f5_prefix_candidates(pfx, expanded_terms);
        for (const std::string_view t : expanded_terms) {
            const auto [it, inserted] = term_seen.emplace(t, 1u);
            if (inserted) {
                dedup_terms.push_back(it->first);
            }
        }
    }
    for (const std::string& sub : plan.substring_terms) {
        if (sub.size() < 3) {
            os << "[Hint] 子串检索建议至少 3 个字符，已忽略: ~" << sub << "~\n";
            continue;
        }
        collect_f5_substring_candidates(sub, expanded_terms);
        for (const std::string_view t : expanded_terms) {
            const auto [it, inserted] = term_seen.emplace(t, 1u);
            if (inserted) {
                dedup_terms.push_back(it->first);
            }
        }
    }

    if (dedup_terms.empty()) {
        if (opts.emit_results) {
            os << "无有效查询词。\n";
        }
        if (profile != nullptr) {
            profile->path = F5ExecPath::InvalidQuery;
            profile->rewrite_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_rewrite0).count();
            profile->total_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_wall0).count();
        }
        active_profile_ = nullptr;
        return;
    }

    std::vector<std::string_view> filtered_terms;
    filtered_terms.reserve(dedup_terms.size());
    for (const std::string_view t : dedup_terms) {
        if (!is_f5_query_stopword(t)) {
            filtered_terms.push_back(t);
        }
    }
    std::vector<std::string_view> terms = filtered_terms.empty() ? dedup_terms : filtered_terms;
    if (terms.empty()) {
        if (opts.emit_results) {
            os << "无有效查询词。\n";
        }
        if (profile != nullptr) {
            profile->path = F5ExecPath::InvalidQuery;
            profile->rewrite_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_rewrite0).count();
            profile->total_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_wall0).count();
        }
        active_profile_ = nullptr;
        return;
    }
    if (profile != nullptr) {
        profile->raw_query_terms = terms.size();
    }

    struct F5ScoringTerm {
        std::string_view term;
        float query_boost = 1.0f;
    };
    std::vector<F5ScoringTerm> scoring_terms;
    scoring_terms.reserve(terms.size() + 16);
    std::unordered_map<std::string_view, std::size_t> scoring_term_pos;
    scoring_term_pos.reserve(terms.size() * 2 + 16);
    auto add_scoring_term = [&](std::string_view term, float boost) {
        auto it = scoring_term_pos.find(term);
        if (it == scoring_term_pos.end()) {
            const std::size_t pos = scoring_terms.size();
            scoring_terms.push_back(F5ScoringTerm{term, boost});
            scoring_term_pos.emplace(term, pos);
        } else {
            F5ScoringTerm& ref = scoring_terms[it->second];
            ref.query_boost = std::max(ref.query_boost, boost);
        }
    };

    std::vector<std::pair<std::string_view, float>> fuzzy_terms;
    std::size_t fuzzy_rewrite_count = 0;
    for (const std::string_view term : terms) {
        const std::vector<Posting>* exact = get_keyword_postings(term);
        if (exact != nullptr && !exact->empty()) {
            add_scoring_term(term, 1.0f);
            continue;
        }
        if (!plan.fuzzy_enabled || term.size() < 3 || term.size() > 32 || is_f5_query_stopword(term)) {
            if (plan.boolean_mode == F5BooleanMode::And) {
                os << "未找到匹配结果（AND 模式下有查询词无命中）。\n";
                return;
            }
            continue;
        }

        fuzzy_terms.clear();
        collect_f5_fuzzy_candidates(term, plan.fuzzy_max_edits, fuzzy_terms);
        if (fuzzy_terms.empty()) {
            if (plan.boolean_mode == F5BooleanMode::And) {
                os << "未找到匹配结果（AND 模式下有查询词无命中）。\n";
                return;
            }
            continue;
        }
        ++fuzzy_rewrite_count;
        const std::size_t adaptive_cap =
            term.size() <= 5 ? std::min<std::size_t>(3, plan.fuzzy_max_expansions) : plan.fuzzy_max_expansions;
        const std::size_t cap = (plan.boolean_mode == F5BooleanMode::And) ? 1 : adaptive_cap;
        std::size_t used = 0;
        for (const auto& fv : fuzzy_terms) {
            add_scoring_term(fv.first, fv.second);
            ++used;
            if (used >= cap) {
                break;
            }
        }
        os << "[Fuzzy] " << term << " -> " << fuzzy_terms.front().first << '\n';
    }

    if (scoring_terms.empty()) {
        if (opts.emit_results) {
            os << "未找到匹配结果。\n";
        }
        if (profile != nullptr) {
            profile->path = F5ExecPath::NoHits;
            profile->rewrite_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_rewrite0).count();
            profile->total_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_wall0).count();
        }
        active_profile_ = nullptr;
        return;
    }
    if (profile != nullptr) {
        profile->scoring_terms = scoring_terms.size();
        profile->fuzzy_rewrite_count = fuzzy_rewrite_count;
        profile->rewrite_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_rewrite0).count();
    }

    std::size_t requested_topk = 0;
    if (plan.page > 0 || plan.size > 0) {
        const std::size_t page_size_req = plan.size > 0 ? plan.size : (plan.limit > 0 ? plan.limit : 20);
        const std::size_t page_req = plan.page > 0 ? plan.page : 1;
        requested_topk = page_req * page_size_req;
    } else if (plan.limit > 0) {
        requested_topk = plan.offset + plan.limit;
    }
    const bool can_use_single_term_blockmax =
        scoring_terms.size() == 1 &&
        plan.boolean_mode == F5BooleanMode::Or &&
        plan.sort_mode == F5SortMode::Relevance &&
        plan.phrases.empty() &&
        requested_topk > 0;
    if (profile != nullptr) {
        profile->requested_topk = requested_topk;
    }

    const bool can_use_newest_year =
        !scoring_terms.empty() &&
        scoring_terms.size() <= 16 &&
        plan.sort_mode == F5SortMode::Newest &&
        plan.phrases.empty() &&
        plan.prefix_terms.empty() &&
        plan.substring_terms.empty() &&
        requested_topk > 0;
    if (can_use_newest_year) {
        std::vector<std::pair<std::string_view, float>> newest_terms;
        newest_terms.reserve(scoring_terms.size());
        for (const F5ScoringTerm& st : scoring_terms) {
            newest_terms.push_back({st.term, st.query_boost});
        }
        std::vector<ScoreDoc> fast_ordered;
        std::size_t fast_total_hits = 0;
        const bool require_all_terms = (plan.boolean_mode == F5BooleanMode::And);
        const auto t_score0 = std::chrono::steady_clock::now();
        if (try_search_f5_newest_year(newest_terms, require_all_terms, requested_topk, fast_ordered, fast_total_hits,
                                      profile)) {
            if (profile != nullptr) {
                profile->score_ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_score0).count();
            }
            emit_ranked_results(fast_ordered, fast_total_hits, scoring_terms.size(), fuzzy_rewrite_count, false,
                                F5ExecPath::NewestYear);

            F5ResultCacheEntry entry;
            entry.total_hits = fast_total_hits;
            entry.matched_query_terms = scoring_terms.size();
            entry.fuzzy_rewrite_count = fuzzy_rewrite_count;
            const std::size_t keep_docs = std::min<std::size_t>(fast_ordered.size(), k_f5_result_cache_doc_cap);
            entry.ordered_prefix.assign(fast_ordered.begin(),
                                        fast_ordered.begin() + static_cast<std::ptrdiff_t>(keep_docs));
            auto cache_it = f5_result_cache_.find(cache_key);
            if (cache_it == f5_result_cache_.end()) {
                while (f5_result_cache_.size() >= k_f5_result_cache_cap && !f5_result_cache_fifo_.empty()) {
                    const std::string evict = std::move(f5_result_cache_fifo_.front());
                    f5_result_cache_fifo_.pop_front();
                    f5_result_cache_.erase(evict);
                }
                f5_result_cache_fifo_.push_back(cache_key);
                f5_result_cache_.emplace(cache_key, std::move(entry));
            } else {
                cache_it->second = std::move(entry);
            }
            if (profile != nullptr) {
                profile->total_ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_wall0).count();
            }
            active_profile_ = nullptr;
            return;
        }
    }

    constexpr std::size_t k_daat_max_terms = 16;
    constexpr std::size_t k_prefix_daat_max_terms = 64;
    const bool is_prefix_relevance_query =
        !plan.prefix_terms.empty() &&
        plan.substring_terms.empty() &&
        plan.sort_mode == F5SortMode::Relevance &&
        plan.boolean_mode == F5BooleanMode::Or &&
        plan.phrases.empty() &&
        requested_topk > 0;
    if (is_prefix_relevance_query && scoring_terms.size() > k_prefix_daat_max_terms &&
        scoring_terms.size() <= k_prefix_daat_max_terms) {
        std::vector<std::pair<std::string_view, float>> prefix_terms;
        prefix_terms.reserve(scoring_terms.size());
        for (const F5ScoringTerm& st : scoring_terms) {
            prefix_terms.push_back({st.term, st.query_boost});
        }
        std::vector<ScoreDoc> fast_ordered;
        std::size_t fast_total_hits = 0;
        const auto t_score0 = std::chrono::steady_clock::now();
        if (try_search_f5_prefix_topk(prefix_terms, requested_topk, fast_ordered, fast_total_hits, profile)) {
            if (profile != nullptr) {
                profile->score_ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_score0).count();
            }
            emit_ranked_results(fast_ordered, fast_total_hits, scoring_terms.size(), fuzzy_rewrite_count, false,
                                F5ExecPath::PrefixDaat);

            F5ResultCacheEntry entry;
            entry.total_hits = fast_total_hits;
            entry.matched_query_terms = scoring_terms.size();
            entry.fuzzy_rewrite_count = fuzzy_rewrite_count;
            const std::size_t keep_docs = std::min<std::size_t>(fast_ordered.size(), k_f5_result_cache_doc_cap);
            entry.ordered_prefix.assign(fast_ordered.begin(),
                                        fast_ordered.begin() + static_cast<std::ptrdiff_t>(keep_docs));
            auto cache_it = f5_result_cache_.find(cache_key);
            if (cache_it == f5_result_cache_.end()) {
                while (f5_result_cache_.size() >= k_f5_result_cache_cap && !f5_result_cache_fifo_.empty()) {
                    const std::string evict = std::move(f5_result_cache_fifo_.front());
                    f5_result_cache_fifo_.pop_front();
                    f5_result_cache_.erase(evict);
                }
                f5_result_cache_fifo_.push_back(cache_key);
                f5_result_cache_.emplace(cache_key, std::move(entry));
            } else {
                cache_it->second = std::move(entry);
            }
            if (profile != nullptr) {
                profile->total_ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_wall0).count();
            }
            active_profile_ = nullptr;
            return;
        }
    }

    const bool can_use_daat_wand =
        !scoring_terms.empty() &&
        (scoring_terms.size() <= k_daat_max_terms ||
         (!plan.prefix_terms.empty() && plan.substring_terms.empty() &&
          scoring_terms.size() <= k_prefix_daat_max_terms)) &&
        plan.sort_mode == F5SortMode::Relevance &&
        (plan.phrases.empty() || plan.boolean_mode == F5BooleanMode::Or) &&
        requested_topk > 0;
    if (can_use_daat_wand) {
        std::vector<std::pair<std::string_view, float>> wand_terms;
        wand_terms.reserve(scoring_terms.size());
        for (const F5ScoringTerm& st : scoring_terms) {
            wand_terms.push_back({st.term, st.query_boost});
        }
        std::vector<ScoreDoc> fast_ordered;
        std::size_t fast_total_hits = 0;
        const bool require_all_terms = (plan.boolean_mode == F5BooleanMode::And);
        std::size_t fast_requested_topk = requested_topk;
        if (!plan.phrases.empty()) {
            constexpr std::size_t kPhraseCandidateMultiplier = 16;
            constexpr std::size_t kPhraseCandidateFloor = 256;
            constexpr std::size_t kPhraseCandidateCap = 4096;
            fast_requested_topk = std::min<std::size_t>(
                kPhraseCandidateCap,
                std::max<std::size_t>(requested_topk * kPhraseCandidateMultiplier, kPhraseCandidateFloor));
        }
        const auto t_score0 = std::chrono::steady_clock::now();
        if (try_search_f5_daat_wand(wand_terms, require_all_terms, fast_requested_topk, fast_ordered, fast_total_hits,
                                    profile)) {
            if (!plan.phrases.empty()) {
                constexpr float kPhraseBoost = 0.85f;
                StringArena phrase_check_arena(256u * 1024u);
                std::size_t checked_docs = 0;
                std::vector<ScoreDoc> phrase_hits;
                std::vector<ScoreDoc> phrase_fillers;
                phrase_hits.reserve(fast_ordered.size());
                phrase_fillers.reserve(fast_ordered.size());
                for (ScoreDoc& e : fast_ordered) {
                    if (e.second >= forward_index_.size()) {
                        phrase_fillers.push_back(e);
                        continue;
                    }
                    const Document& doc = forward_index_[e.second];
                    if (doc.title.empty()) {
                        phrase_fillers.push_back(e);
                        continue;
                    }
                    const std::vector<std::string_view> doc_terms =
                        Analyzer::normalize_and_tokenize(doc.title, phrase_check_arena);
                    if (doc_terms.empty()) {
                        phrase_fillers.push_back(e);
                        continue;
                    }
                    float phrase_gain = 0.0f;
                    for (const auto& phrase : plan.phrases) {
                        if (contains_phrase(doc_terms, phrase)) {
                            phrase_gain += kPhraseBoost * static_cast<float>(phrase.size());
                        }
                    }
                    e.first += phrase_gain;
                    if (phrase_gain > 0.0f) {
                        phrase_hits.push_back(e);
                    } else {
                        phrase_fillers.push_back(e);
                    }
                    ++checked_docs;
                    if ((checked_docs & 1023u) == 0u) {
                        phrase_check_arena.reset();
                    }
                }
                sort_f5_daat_results(phrase_hits);
                sort_f5_daat_results(phrase_fillers);
                fast_ordered.clear();
                fast_ordered.reserve(std::min<std::size_t>(
                    requested_topk, phrase_hits.size() + phrase_fillers.size()));
                for (const ScoreDoc& e : phrase_hits) {
                    if (fast_ordered.size() >= requested_topk) {
                        break;
                    }
                    fast_ordered.push_back(e);
                }
                for (const ScoreDoc& e : phrase_fillers) {
                    if (fast_ordered.size() >= requested_topk) {
                        break;
                    }
                    fast_ordered.push_back(e);
                }
                if (fast_ordered.size() > requested_topk) {
                    fast_ordered.resize(requested_topk);
                }
            }
            if (profile != nullptr) {
                profile->score_ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_score0).count();
            }
            const F5ExecPath daat_path =
                !plan.phrases.empty() ? F5ExecPath::PhraseDaat
                : !plan.prefix_terms.empty() ? F5ExecPath::PrefixDaat
                                      : (require_all_terms ? F5ExecPath::DaatWandAnd : F5ExecPath::DaatWandOr);
            emit_ranked_results(fast_ordered, fast_total_hits, scoring_terms.size(), fuzzy_rewrite_count, false,
                                daat_path);

            F5ResultCacheEntry entry;
            entry.total_hits = fast_total_hits;
            entry.matched_query_terms = scoring_terms.size();
            entry.fuzzy_rewrite_count = fuzzy_rewrite_count;
            const std::size_t keep_docs = std::min<std::size_t>(fast_ordered.size(), k_f5_result_cache_doc_cap);
            entry.ordered_prefix.assign(fast_ordered.begin(),
                                        fast_ordered.begin() + static_cast<std::ptrdiff_t>(keep_docs));
            auto cache_it = f5_result_cache_.find(cache_key);
            if (cache_it == f5_result_cache_.end()) {
                while (f5_result_cache_.size() >= k_f5_result_cache_cap && !f5_result_cache_fifo_.empty()) {
                    const std::string evict = std::move(f5_result_cache_fifo_.front());
                    f5_result_cache_fifo_.pop_front();
                    f5_result_cache_.erase(evict);
                }
                f5_result_cache_fifo_.push_back(cache_key);
                f5_result_cache_.emplace(cache_key, std::move(entry));
            } else {
                cache_it->second = std::move(entry);
            }
            if (profile != nullptr) {
                profile->total_ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_wall0).count();
            }
            active_profile_ = nullptr;
            return;
        }
    }

    if (can_use_single_term_blockmax) {
        std::vector<ScoreDoc> fast_ordered;
        std::size_t fast_total_hits = 0;
        const auto t_score0 = std::chrono::steady_clock::now();
        if (try_search_f5_single_term_blockmax(scoring_terms.front().term, scoring_terms.front().query_boost,
                                               requested_topk, fast_ordered, fast_total_hits, profile)) {
            if (profile != nullptr) {
                profile->score_ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_score0).count();
            }
            emit_ranked_results(fast_ordered, fast_total_hits, 1u, fuzzy_rewrite_count, false,
                                F5ExecPath::SingleTermBlockmax);

            F5ResultCacheEntry entry;
            entry.total_hits = fast_total_hits;
            entry.matched_query_terms = 1u;
            entry.fuzzy_rewrite_count = fuzzy_rewrite_count;
            const std::size_t keep_docs = std::min<std::size_t>(fast_ordered.size(), k_f5_result_cache_doc_cap);
            entry.ordered_prefix.assign(fast_ordered.begin(), fast_ordered.begin() + static_cast<std::ptrdiff_t>(keep_docs));
            auto cache_it = f5_result_cache_.find(cache_key);
            if (cache_it == f5_result_cache_.end()) {
                while (f5_result_cache_.size() >= k_f5_result_cache_cap && !f5_result_cache_fifo_.empty()) {
                    const std::string evict = std::move(f5_result_cache_fifo_.front());
                    f5_result_cache_fifo_.pop_front();
                    f5_result_cache_.erase(evict);
                }
                f5_result_cache_fifo_.push_back(cache_key);
                f5_result_cache_.emplace(cache_key, std::move(entry));
            } else {
                cache_it->second = std::move(entry);
            }
            if (profile != nullptr) {
                profile->total_ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_wall0).count();
            }
            active_profile_ = nullptr;
            return;
        }
    }
    const bool can_use_or_blockmax =
        scoring_terms.size() > 1 &&
        scoring_terms.size() <= 8 &&
        plan.boolean_mode == F5BooleanMode::Or &&
        plan.sort_mode == F5SortMode::Relevance &&
        plan.phrases.empty() &&
        requested_topk > 0;
    if (can_use_or_blockmax) {
        std::vector<std::pair<std::string_view, float>> wand_terms;
        wand_terms.reserve(scoring_terms.size());
        for (const F5ScoringTerm& st : scoring_terms) {
            wand_terms.push_back({st.term, st.query_boost});
        }
        std::vector<ScoreDoc> fast_ordered;
        std::size_t fast_total_hits = 0;
        const auto t_score0 = std::chrono::steady_clock::now();
        if (try_search_f5_or_blockmax(wand_terms, requested_topk, fast_ordered, fast_total_hits, profile)) {
            if (profile != nullptr) {
                profile->score_ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_score0).count();
            }
            emit_ranked_results(fast_ordered, fast_total_hits, scoring_terms.size(), fuzzy_rewrite_count, false,
                                F5ExecPath::OrBlockmax);

            F5ResultCacheEntry entry;
            entry.total_hits = fast_total_hits;
            entry.matched_query_terms = scoring_terms.size();
            entry.fuzzy_rewrite_count = fuzzy_rewrite_count;
            const std::size_t keep_docs = std::min<std::size_t>(fast_ordered.size(), k_f5_result_cache_doc_cap);
            entry.ordered_prefix.assign(fast_ordered.begin(), fast_ordered.begin() + static_cast<std::ptrdiff_t>(keep_docs));
            auto cache_it = f5_result_cache_.find(cache_key);
            if (cache_it == f5_result_cache_.end()) {
                while (f5_result_cache_.size() >= k_f5_result_cache_cap && !f5_result_cache_fifo_.empty()) {
                    const std::string evict = std::move(f5_result_cache_fifo_.front());
                    f5_result_cache_fifo_.pop_front();
                    f5_result_cache_.erase(evict);
                }
                f5_result_cache_fifo_.push_back(cache_key);
                f5_result_cache_.emplace(cache_key, std::move(entry));
            } else {
                cache_it->second = std::move(entry);
            }
            if (profile != nullptr) {
                profile->total_ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_wall0).count();
            }
            active_profile_ = nullptr;
            return;
        }
    }

    const auto t_score0 = std::chrono::steady_clock::now();
    std::vector<DocID> modified_docs;
    modified_docs.reserve(4096);
    std::unordered_map<DocID, std::uint16_t> and_hits;
    if (plan.boolean_mode == F5BooleanMode::And) {
        and_hits.reserve(4096);
    }

    const double N = static_cast<double>(n);
    std::size_t matched_query_terms = 0;

    for (const F5ScoringTerm& qterm : scoring_terms) {
        const std::string_view term = qterm.term;
        const std::vector<Posting>* plist = get_keyword_postings(term);
        if (plist == nullptr || plist->empty()) {
            continue;
        }
        ++matched_query_terms;
        const double df = static_cast<double>(plist->size());
        const double df_ratio = (N > 0.0) ? (df / N) : 0.0;
        const float penalty = f5_high_df_penalty(df_ratio) * (is_f5_query_stopword(term) ? 0.35f : 1.0f);
        const float idf = static_cast<float>(std::log((N - df + 0.5) / (df + 0.5) + 1.0)) * penalty * qterm.query_boost;

        if (profile != nullptr) {
            profile->postings_visited += plist->size();
        }
        for (const Posting& po : *plist) {
            const DocID did = po.doc_id;
            if (did >= forward_index_.size()) {
                continue;
            }
            if (profile != nullptr) {
                profile->postings_scored += 1;
            }
            const float dl =
                static_cast<float>(std::max<std::uint32_t>(1u, forward_index_[did].doc_length));
            const float avdl = avg_dl_ > 0.0f ? avg_dl_ : 1.0f;
            const float K = k_bm25_k1 * ((1.0f - k_bm25_b) + k_bm25_b * (dl / avdl));
            const float tf = static_cast<float>(po.tf);
            const float inc = idf * (tf * (k_bm25_k1 + 1.0f)) / (tf + K);

            const float prev = scoring_board_[did];
            scoring_board_[did] = prev + inc;
            if (prev == 0.0f) {
                modified_docs.push_back(did);
            }
            if (plan.boolean_mode == F5BooleanMode::And) {
                ++and_hits[did];
            }
        }
    }
    if (profile != nullptr) {
        profile->score_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_score0).count();
        profile->docs_touched = modified_docs.size();
    }

    if (matched_query_terms == 0) {
        if (opts.emit_results) {
            os << "未找到匹配结果。\n";
        }
        if (profile != nullptr) {
            profile->path = F5ExecPath::NoHits;
            profile->total_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_wall0).count();
        }
        active_profile_ = nullptr;
        return;
    }

    if (!plan.phrases.empty() && !modified_docs.empty()) {
        // Phrase hits provide an extra relevance signal on top of BM25.
        constexpr float kPhraseBoost = 0.85f;
        StringArena phrase_check_arena(256u * 1024u);
        std::size_t checked_docs = 0;
        for (const DocID did : modified_docs) {
            if (did >= forward_index_.size()) {
                continue;
            }
            const Document& doc = forward_index_[did];
            if (doc.title.empty()) {
                continue;
            }
            const std::vector<std::string_view> doc_terms = Analyzer::normalize_and_tokenize(doc.title, phrase_check_arena);
            if (doc_terms.empty()) {
                continue;
            }
            float phrase_gain = 0.0f;
            for (const auto& phrase : plan.phrases) {
                if (contains_phrase(doc_terms, phrase)) {
                    phrase_gain += kPhraseBoost * static_cast<float>(phrase.size());
                }
            }
            scoring_board_[did] += phrase_gain;
            ++checked_docs;
            if ((checked_docs & 1023u) == 0u) {
                phrase_check_arena.reset();
            }
        }
    }

    std::vector<ScoreDoc> ordered;
    ordered.reserve(modified_docs.size());
    for (const DocID did : modified_docs) {
        if (plan.boolean_mode == F5BooleanMode::And) {
            const auto it = and_hits.find(did);
            if (it == and_hits.end() || it->second != matched_query_terms) {
                continue;
            }
        }
        const float s = scoring_board_[did];
        ordered.push_back({s, did});
    }

    if (ordered.empty()) {
        if (opts.emit_results) {
            os << "未找到匹配结果。\n";
        }
        for (const DocID did : modified_docs) {
            scoring_board_[did] = 0.0f;
        }
        if (profile != nullptr) {
            profile->path = F5ExecPath::NoHits;
            profile->total_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_wall0).count();
        }
        active_profile_ = nullptr;
        return;
    }

    const auto t_rank0 = std::chrono::steady_clock::now();
    const auto better = [&](const ScoreDoc& a, const ScoreDoc& b) {
        if (plan.sort_mode == F5SortMode::Newest) {
            const int ya = forward_index_[a.second].year;
            const int yb = forward_index_[b.second].year;
            if (ya != yb) {
                return ya > yb;
            }
            if (a.first != b.first) {
                return a.first > b.first;
            }
            return a.second < b.second;
        }
        if (a.first != b.first) {
            return a.first > b.first;
        }
        const int ya = forward_index_[a.second].year;
        const int yb = forward_index_[b.second].year;
        if (ya != yb) {
            return ya > yb;
        }
        return a.second < b.second;
    };

    const std::size_t total_hits = ordered.size();
    std::size_t needed_topk = total_hits;
    if (plan.page > 0 || plan.size > 0) {
        const std::size_t page_size_req = plan.size > 0 ? plan.size : (plan.limit > 0 ? plan.limit : 20);
        const std::size_t page_req = plan.page > 0 ? plan.page : 1;
        needed_topk = std::min<std::size_t>(total_hits, page_req * page_size_req);
    } else if (plan.limit > 0) {
        needed_topk = std::min<std::size_t>(total_hits, plan.offset + plan.limit);
    }
    if (needed_topk < ordered.size()) {
        std::nth_element(ordered.begin(), ordered.begin() + static_cast<std::ptrdiff_t>(needed_topk), ordered.end(), better);
        ordered.resize(needed_topk);
    }
    std::sort(ordered.begin(), ordered.end(), better);
    if (profile != nullptr) {
        profile->rank_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_rank0).count();
        profile->matched_query_terms = matched_query_terms;
    }

    emit_ranked_results(ordered, total_hits, matched_query_terms, fuzzy_rewrite_count, false, F5ExecPath::FullScan);

    F5ResultCacheEntry entry;
    entry.total_hits = total_hits;
    entry.matched_query_terms = matched_query_terms;
    entry.fuzzy_rewrite_count = fuzzy_rewrite_count;
    const std::size_t keep_docs = std::min<std::size_t>(ordered.size(), k_f5_result_cache_doc_cap);
    entry.ordered_prefix.assign(ordered.begin(), ordered.begin() + static_cast<std::ptrdiff_t>(keep_docs));
    auto cache_it = f5_result_cache_.find(cache_key);
    if (cache_it == f5_result_cache_.end()) {
        while (f5_result_cache_.size() >= k_f5_result_cache_cap && !f5_result_cache_fifo_.empty()) {
            const std::string evict = std::move(f5_result_cache_fifo_.front());
            f5_result_cache_fifo_.pop_front();
            f5_result_cache_.erase(evict);
        }
        f5_result_cache_fifo_.push_back(cache_key);
        f5_result_cache_.emplace(cache_key, std::move(entry));
    } else {
        cache_it->second = std::move(entry);
    }

    for (const DocID did : modified_docs) {
        scoring_board_[did] = 0.0f;
    }

    if (profile != nullptr) {
        profile->total_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_wall0).count();
    }
    active_profile_ = nullptr;
}

void ExtremeEngine::execute_f3_author_stats() const {
    const auto t0 = std::chrono::steady_clock::now();

    const auto t_compute = std::chrono::steady_clock::now();
    const auto compute_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t_compute - t0).count();

    std::cout << "\n========== Top 100 Prolific Authors ==========\n";
    std::cout << "[计时] 统计耗时: " << compute_ms << " ms\n\n";

    if (f3_top100_cache_.empty()) {
        std::cout << "（暂无作者索引数据）\n";
    } else {
        std::size_t rank = 1;
        for (const auto& entry : f3_top100_cache_) {
            std::cout << '[' << rank << "] " << entry.second << ": " << entry.first << " papers\n";
            ++rank;
        }
    }

    const auto t_done = std::chrono::steady_clock::now();
    const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_done - t0).count();
    std::cout << "\n[计时] 总耗时（含输出）: " << total_ms << " ms\n";
}

void ExtremeEngine::execute_f4_conference_analytics() const {
    const auto t0 = std::chrono::steady_clock::now();

    std::cout << "请输入目标年份（如 2023）: ";
    int target_year = 0;
    if (!(std::cin >> target_year)) {
        std::cin.clear();
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        std::cout << "输入无效，年份应为整数。\n";
        return;
    }
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

    const auto docs_it = year_doc_index_.find(target_year);
    const std::size_t matched_docs = docs_it == year_doc_index_.end() ? 0 : docs_it->second.size();

    auto cache_it = f4_top10_cache_.find(target_year);
    if (cache_it == f4_top10_cache_.end()) {
        cache_it = f4_top10_cache_.emplace(target_year, compute_f4_top10_for_year(target_year)).first;
    }
    const std::vector<F4HotEntry>& ranked = cache_it->second;

    std::cout << "\n========== Annual Trending Words ==========\n";
    std::cout << "[Year] " << target_year << '\n';
    std::cout << "[Matched Documents] " << matched_docs << "\n\n";

    if (ranked.empty()) {
        std::cout << "未统计到可用热词。\n";
    } else {
        std::size_t rank = 1;
        for (const auto& entry : ranked) {
            std::cout << '[' << rank << "] keyword: " << entry.term << " (Frequency: " << entry.freq << ")\n";

            if (!kF4FastModeDisableEvidence) {
                constexpr std::size_t kEvidenceLimit = 3;
                if (const std::vector<Posting>* plist = keyword_global_.find(entry.term); plist != nullptr) {
                    std::size_t emitted = 0;
                    for (const Posting& posting : *plist) {
                        if (posting.doc_id >= forward_index_.size()) {
                            continue;
                        }
                        const Document& doc = forward_index_[posting.doc_id];
                        if (doc.year != target_year || doc.title.empty()) {
                            continue;
                        }
                        std::cout << "    - evidence: " << doc.title << '\n';
                        ++emitted;
                        if (emitted >= kEvidenceLimit) {
                            break;
                        }
                    }
                }
            }
            ++rank;
        }
        if (kF4FastModeDisableEvidence) {
            std::cout << "\n[Fast Mode] 证据标题回扫已禁用，以压缩 F4 到亚秒级响应。\n";
        }
    }

    const auto t1 = std::chrono::steady_clock::now();
    const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::cout << "\n[计时] F4 执行总耗时: " << total_ms << " ms\n";
}

void ExtremeEngine::execute_f6_global_ranking() const {
    const auto t0 = std::chrono::steady_clock::now();

    constexpr std::size_t kMaxAuthorsPerPaper = 64;
    constexpr unsigned long long kCountSaturation = std::numeric_limits<unsigned long long>::max();

    std::cout << "========== F6 全图聚团统计 ==========\n";
    std::cout << "[Mode] Degeneracy Ordering + DAG 重定向 + 递归有序交集，统计到最大可达阶。\n";

    std::unordered_map<std::string_view, std::uint32_t> author_id;
    author_id.reserve(author_global_.size() * 2 + 1);
    std::uint32_t next_id = 0;
    author_global_.for_each([&](const std::string_view author, const std::vector<Posting>&) {
        if (!author.empty()) {
            author_id.emplace(author, next_id++);
        }
    });

    const std::uint32_t author_count = next_id;
    if (author_count == 0) {
        std::cout << "作者图为空。\n";
        return;
    }

    std::vector<std::uint64_t> encoded_edges;
    encoded_edges.reserve(std::min<std::size_t>(forward_index_.size() * 2, 32u * 1024u * 1024u));

    std::uint64_t papers_with_authors = 0;
    std::uint64_t skipped_large_papers = 0;
    std::size_t max_authors_seen = 0;
    for (const Document& doc : forward_index_) {
        if (doc.authors.empty()) {
            continue;
        }
        std::vector<std::uint32_t> ids;
        ids.reserve(8);
        for_each_author_segment(doc.authors, [&](const std::string_view seg) {
            const std::string_view trimmed = trim_sv(seg);
            if (trimmed.empty()) {
                return;
            }
            const std::vector<std::string_view> toks = Analyzer::normalize_and_tokenize(trimmed, query_arena_);
            if (toks.empty()) {
                return;
            }
            const char* const beg = toks.front().data();
            const char* const ed = toks.back().data() + toks.back().size();
            const std::string_view key{beg, static_cast<std::size_t>(ed - beg)};
            const auto it = author_id.find(key);
            if (it != author_id.end()) {
                ids.push_back(it->second);
            }
        });
        if (ids.empty()) {
            continue;
        }
        ++papers_with_authors;
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
        max_authors_seen = std::max(max_authors_seen, ids.size());
        if (ids.size() > kMaxAuthorsPerPaper) {
            ++skipped_large_papers;
            ids.resize(kMaxAuthorsPerPaper);
        }
        for (std::size_t i = 0; i < ids.size(); ++i) {
            for (std::size_t j = i + 1; j < ids.size(); ++j) {
                const std::uint32_t u = ids[i];
                const std::uint32_t v = ids[j];
                const std::uint32_t a = std::min(u, v);
                const std::uint32_t b = std::max(u, v);
                encoded_edges.push_back((static_cast<std::uint64_t>(a) << 32u) | b);
            }
        }
        query_arena_.reset();
    }

    std::sort(encoded_edges.begin(), encoded_edges.end());
    encoded_edges.erase(std::unique(encoded_edges.begin(), encoded_edges.end()), encoded_edges.end());
    std::vector<std::vector<std::uint32_t>> adj(author_count);
    for (const std::uint64_t e : encoded_edges) {
        const std::uint32_t u = static_cast<std::uint32_t>(e >> 32u);
        const std::uint32_t v = static_cast<std::uint32_t>(e);
        adj[u].push_back(v);
        adj[v].push_back(u);
    }
    for (std::vector<std::uint32_t>& nbrs : adj) {
        std::sort(nbrs.begin(), nbrs.end());
    }

    std::vector<std::uint32_t> core_degree(author_count, 0u);
    for (const std::uint64_t e : encoded_edges) {
        const std::uint32_t u = static_cast<std::uint32_t>(e >> 32u);
        const std::uint32_t v = static_cast<std::uint32_t>(e);
        ++core_degree[u];
        ++core_degree[v];
    }

    using DegNode = std::pair<std::uint32_t, std::uint32_t>;
    std::priority_queue<DegNode, std::vector<DegNode>, std::greater<DegNode>> peel_heap;
    for (std::uint32_t v = 0; v < author_count; ++v) {
        peel_heap.push({core_degree[v], v});
    }
    std::vector<std::uint8_t> removed(author_count, 0u);
    std::vector<std::uint32_t> rank(author_count, 0u);
    std::uint32_t rank_next = 0;
    std::uint32_t degeneracy = 0;
    while (!peel_heap.empty()) {
        const auto [deg, v] = peel_heap.top();
        peel_heap.pop();
        if (removed[v] != 0u || deg != core_degree[v]) {
            continue;
        }
        removed[v] = 1u;
        rank[v] = rank_next++;
        degeneracy = std::max(degeneracy, deg);
        for (const std::uint32_t nb : adj[v]) {
            if (removed[nb] == 0u && core_degree[nb] > 0u) {
                --core_degree[nb];
                peel_heap.push({core_degree[nb], nb});
            }
        }
    }

    std::vector<std::vector<std::uint32_t>> out(author_count);
    for (const std::uint64_t e : encoded_edges) {
        const std::uint32_t u = static_cast<std::uint32_t>(e >> 32u);
        const std::uint32_t v = static_cast<std::uint32_t>(e);
        if (rank[u] < rank[v]) {
            out[u].push_back(v);
        } else {
            out[v].push_back(u);
        }
    }
    adj.clear();
    adj.shrink_to_fit();
    for (std::vector<std::uint32_t>& nbrs : out) {
        std::sort(nbrs.begin(), nbrs.end(), [&](const std::uint32_t a, const std::uint32_t b) {
            return rank[a] < rank[b];
        });
    }

    const unsigned hw_threads = std::max(1u, std::thread::hardware_concurrency());
    const unsigned worker_count = std::max(1u, std::min<unsigned>(16u, hw_threads));
    constexpr std::uint32_t kVertexBatch = 256;
    std::atomic<std::uint32_t> next_vertex{0u};

    struct ThreadCliqueCounts {
        std::vector<unsigned long long> counts{0ULL, 0ULL};
        std::vector<std::uint8_t> saturated{0u, 0u};
    };
    std::vector<ThreadCliqueCounts> thread_counts(worker_count);

    auto add_local_count = [&](ThreadCliqueCounts& local, const std::uint32_t order, const unsigned long long delta) {
        if (order >= local.counts.size()) {
            local.counts.resize(static_cast<std::size_t>(order) + 1, 0ULL);
            local.saturated.resize(static_cast<std::size_t>(order) + 1, 0u);
        }
        if (kCountSaturation - local.counts[order] < delta) {
            local.counts[order] = kCountSaturation;
            local.saturated[order] = 1u;
        } else {
            local.counts[order] += delta;
        }
    };

    auto worker = [&](const unsigned tid) {
        ThreadCliqueCounts& local = thread_counts[tid];
        auto intersect_sorted_local = [&](const std::vector<std::uint32_t>& a, const std::vector<std::uint32_t>& b) {
            std::vector<std::uint32_t> outv;
            outv.reserve(std::min(a.size(), b.size()));
            auto ia = a.begin();
            auto ib = b.begin();
            while (ia != a.end() && ib != b.end()) {
                const std::uint32_t va = *ia;
                const std::uint32_t vb = *ib;
                if (va == vb) {
                    outv.push_back(va);
                    ++ia;
                    ++ib;
                } else if (rank[va] < rank[vb]) {
                    ++ia;
                } else {
                    ++ib;
                }
            }
            return outv;
        };

        std::function<void(std::uint32_t, std::vector<std::uint32_t>&)> count_from;
        count_from = [&](const std::uint32_t depth, std::vector<std::uint32_t>& candidates) {
            if (candidates.empty()) {
                return;
            }
            for (const std::uint32_t v : candidates) {
                add_local_count(local, depth + 1, 1ULL);
                std::vector<std::uint32_t> next = intersect_sorted_local(candidates, out[v]);
                count_from(depth + 1, next);
            }
        };

        while (true) {
            const std::uint32_t begin = next_vertex.fetch_add(kVertexBatch, std::memory_order_relaxed);
            if (begin >= author_count) {
                break;
            }
            const std::uint32_t end = std::min<std::uint32_t>(author_count, begin + kVertexBatch);
            for (std::uint32_t u = begin; u < end; ++u) {
                std::vector<std::uint32_t> candidates = out[u];
                count_from(1, candidates);
            }
        }
    };

    std::cout << "[Parallel] counting_threads=" << worker_count << '\n';
    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (unsigned tid = 0; tid < worker_count; ++tid) {
        workers.emplace_back(worker, tid);
    }
    for (std::thread& th : workers) {
        th.join();
    }

    std::vector<unsigned long long> clique_counts(2, 0ULL);
    std::vector<std::uint8_t> saturated(2, 0u);
    clique_counts[1] = author_count;
    for (const ThreadCliqueCounts& local : thread_counts) {
        if (local.counts.size() > clique_counts.size()) {
            clique_counts.resize(local.counts.size(), 0ULL);
            saturated.resize(local.counts.size(), 0u);
        }
        for (std::size_t k = 2; k < local.counts.size(); ++k) {
            if (local.saturated[k] != 0u || kCountSaturation - clique_counts[k] < local.counts[k]) {
                clique_counts[k] = kCountSaturation;
                saturated[k] = 1u;
            } else {
                clique_counts[k] += local.counts[k];
            }
        }
    }

    std::uint32_t oriented_max_out_degree = 0;
    for (const std::vector<std::uint32_t>& nbrs : out) {
        oriented_max_out_degree =
            std::max<std::uint32_t>(oriented_max_out_degree, static_cast<std::uint32_t>(nbrs.size()));
    }

    std::cout << "[Graph] authors=" << author_count
              << " papers_with_authors=" << papers_with_authors
              << " collaboration_edges=" << encoded_edges.size() << '\n';
    std::cout << "[Graph] max_authors_per_paper_seen=" << max_authors_seen
              << " skipped_or_truncated_large_papers=" << skipped_large_papers
              << " degeneracy=" << degeneracy
              << " oriented_max_out_degree=" << oriented_max_out_degree << '\n';
    std::cout << "[Counts] all complete subgraphs, not only maximal cliques\n";
    std::uint32_t max_order = 0;
    for (std::uint32_t k = 1; k < clique_counts.size(); ++k) {
        if (clique_counts[k] != 0ULL) {
            max_order = k;
        }
    }
    for (std::uint32_t k = 1; k <= max_order; ++k) {
        std::cout << "Order " << k << ": " << clique_counts[k];
        if (k < saturated.size() && saturated[k] != 0u) {
            std::cout << " (saturated)";
        }
        std::cout << '\n';
    }
    std::cout << "[MaxOrder] " << max_order << '\n';

    const auto t1 = std::chrono::steady_clock::now();
    const auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::cout << "[计时] F6 执行总耗时: " << total_ms << " ms\n";
}

void ExtremeEngine::execute_f7_export_report() const { std::cout << "功能敬请期待\n"; }
