#pragma once

#include "Analyzer.h"
#include "Document.h"
#include "Infrastructure.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

struct LocalIndex {
    StringArena arena;
    std::vector<Document> forward_index;
    FlatMap<std::string_view, std::vector<Posting>> author_inverted;
    FlatMap<std::string_view, std::vector<Posting>> title_exact_inverted;
    FlatMap<std::string_view, std::vector<Posting>> keyword_inverted;

    LocalIndex() = default;
    LocalIndex(const LocalIndex&) = delete;
    LocalIndex& operator=(const LocalIndex&) = delete;
    LocalIndex(LocalIndex&&) = default;
    LocalIndex& operator=(LocalIndex&&) = default;
};

class ExtremeParser {
public:
    explicit ExtremeParser(std::size_t max_concurrent_consumers = 0) noexcept;

    [[nodiscard]] std::vector<std::unique_ptr<LocalIndex>> parse_file(const char* utf8_path);

    [[nodiscard]] std::size_t chunk_bytes() const noexcept { return chunk_bytes_; }
    [[nodiscard]] std::size_t max_concurrent() const noexcept { return max_concurrent_; }

private:
    std::size_t chunk_bytes_;
    std::size_t max_concurrent_;
};
