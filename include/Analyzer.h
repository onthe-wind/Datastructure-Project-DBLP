#pragma once

#include <string_view>
#include <vector>

class StringArena;

class Analyzer {
public:
    static std::vector<std::string_view> normalize_and_tokenize(std::string_view raw, StringArena& arena);
};
