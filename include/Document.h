#pragma once

#include <cstdint>
#include <string_view>

using DocID = std::uint32_t;

struct Document {
    DocID id = 0;
    std::string_view title;
    std::string_view authors;
    std::string_view journal;
    int year = 0;
    std::uint32_t doc_length = 0;
    std::string_view volume;
    std::string_view month;
    std::string_view cdrom;
    std::string_view ee;
    std::string_view url;
};

struct Posting {
    DocID doc_id = 0;
    std::uint32_t tf = 0;
};
