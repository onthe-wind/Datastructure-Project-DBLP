#include "ExtremeEngine.h"
#include "ExtremeParser.h"
#include "F5Profile.h"
#include "QuerySuite.h"

#include <chrono>
#include <clocale>
#include <exception>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

[[nodiscard]] std::string default_xml_path() { return "dblp.xml"; }

void print_menu() {
    std::cout << "\n========== DBLP Extreme 文献检索 ==========\n";
    std::cout << " [1] F1. 按作者完整搜索\n";
    std::cout << " [2] F1. 按标题完整搜索\n";
    std::cout << " [3] F2. 挖掘作者合作关系\n";
    std::cout << " [4] F5. 组合关键字打分搜索 (BM25)\n";
    std::cout << " [5] F3. 作者统计功能 (架构预留)\n";
    std::cout << " [6] F4. 年度热词分析 (架构预留)\n";
    std::cout << " [7] F6. 全图聚团统计\n";
    std::cout << " [8] F7. 可视化数据导出 (架构预留)\n";
    std::cout << " [9] F5. 查询评测集基准 (profiling)\n";
    std::cout << " [0] 退出系统\n";
    std::cout << "============================================\n";
    std::cout << "请选择功能编号: " << std::flush;
}

[[nodiscard]] int read_menu_choice() {
    std::string line;
    if (!std::getline(std::cin, line)) {
        return 0;
    }
    try {
        const int v = std::stoi(line);
        return v;
    } catch (const std::exception&) {
        return -1;
    }
}

[[nodiscard]] std::string read_user_line(const char* prompt) {
    std::cout << prompt << std::flush;
    std::string line;
    std::getline(std::cin, line);
    return line;
}

[[nodiscard]] std::string trim_copy(std::string_view s) {
    std::size_t beg = 0;
    while (beg < s.size() && std::isspace(static_cast<unsigned char>(s[beg])) != 0) {
        ++beg;
    }
    std::size_t end = s.size();
    while (end > beg && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) {
        --end;
    }
    return std::string(s.substr(beg, end - beg));
}

[[nodiscard]] int read_option_or_default(const char* prompt, int default_value, int min_value, int max_value) {
    const std::string line = trim_copy(read_user_line(prompt));
    if (line.empty()) {
        return default_value;
    }
    try {
        const int value = std::stoi(line);
        if (value < min_value || value > max_value) {
            return default_value;
        }
        return value;
    } catch (const std::exception&) {
        return default_value;
    }
}

[[nodiscard]] std::string build_f5_search_query() {
    std::cout << "\n========== F5 学术论文搜索 ==========\n";
    std::cout << "像 Nature / IEEE Xplore 一样输入关键词或短语；高级选项可直接回车使用默认值。\n";
    std::cout << "示例: graph neural network | \"deep learning\" recommendation\n\n";

    const std::string keywords = trim_copy(read_user_line("Search papers: "));
    if (keywords.empty()) {
        return {};
    }

    std::cout << "\n排序方式:\n";
    std::cout << " [1] Relevance 相关度优先（默认）\n";
    std::cout << " [2] Newest 最新年份优先\n";
    const int sort = read_option_or_default("请选择排序 [1-2，默认1]: ", 1, 1, 2);

    std::cout << "\n匹配方式:\n";
    std::cout << " [1] Smart 智能匹配（默认，宽召回 + BM25 排名）\n";
    std::cout << " [2] All terms 必须包含所有关键词\n";
    std::cout << " [3] Any terms 任意关键词即可\n";
    const int match_mode = read_option_or_default("请选择匹配方式 [1-3，默认1]: ", 1, 1, 3);

    std::cout << "\n容错搜索:\n";
    std::cout << " [1] Auto 自动纠错（默认）\n";
    std::cout << " [2] Off 关闭纠错\n";
    std::cout << " [3] Broad 更宽松纠错\n";
    const int fuzzy = read_option_or_default("请选择容错级别 [1-3，默认1]: ", 1, 1, 3);

    std::cout << "\n每页结果数:\n";
    std::cout << " [1] 10  [2] 20（默认）  [3] 50\n";
    const int page_size_choice = read_option_or_default("请选择每页数量 [1-3，默认2]: ", 2, 1, 3);
    const int page_size = page_size_choice == 1 ? 10 : (page_size_choice == 3 ? 50 : 20);
    const int page = read_option_or_default("页码 [默认1]: ", 1, 1, 1000000);

    std::ostringstream query;
    query << keywords;
    if (sort == 2) {
        query << " sort:newest";
    }
    if (match_mode == 2) {
        query << " mode:and";
    } else if (match_mode == 3) {
        query << " mode:or";
    }
    if (fuzzy == 2) {
        query << " fuzzy:off";
    } else if (fuzzy == 3) {
        query << " fuzzy:2 fuzzyexp:8";
    } else {
        query << " fuzzy:1 fuzzyexp:4";
    }
    query << " page:" << page << " size:" << page_size;
    return query.str();
}

void run_search_timed(const ExtremeEngine& engine, auto&& fn) {
    const auto t0 = std::chrono::steady_clock::now();
    fn();
    const auto t1 = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::cout << "\n[计时] 本次检索耗时: " << ms << " ms\n";
}

} // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    if (std::setlocale(LC_ALL, ".UTF8") == nullptr) {
        (void)std::setlocale(LC_ALL, "C.UTF-8");
    }

    bool benchmark_mode = false;
    bool rebuild_mode = false;
    std::string xml_path = default_xml_path();
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] != nullptr ? std::string(argv[i]) : std::string{};
        if (arg == "--benchmark" || arg == "-b") {
            benchmark_mode = true;
            continue;
        }
        if (arg == "--rebuild") {
            rebuild_mode = true;
            continue;
        }
        if (!arg.empty() && arg[0] != '-') {
            xml_path = arg;
        }
    }

    std::cout << "数据文件路径: " << xml_path << "\n";

    ExtremeEngine engine;

    try {
        bool loaded_from_segment = false;
        if (!rebuild_mode) {
            const auto t_load_begin = std::chrono::steady_clock::now();
            loaded_from_segment = engine.try_load_serving_index();
            const auto t_load_done = std::chrono::steady_clock::now();
            if (loaded_from_segment) {
                const double load_sec =
                    std::chrono::duration<double>(t_load_done - t_load_begin).count();
                std::cout << "\n[WarmStart] 已加载完整 serving segment，耗时: " << load_sec << " 秒\n";
            }
        }

        if (!loaded_from_segment) {
            if (rebuild_mode) {
                std::cout << "[WarmStart] --rebuild 已启用，跳过 serving segment。\n";
            } else {
                std::cout << "[WarmStart] 未找到或无法加载 serving segment，回退到 XML 构建。\n";
            }
            try {
                if (!std::filesystem::exists(xml_path)) {
                    std::cerr << "错误: 找不到数据文件 \"" << xml_path << "\"。\n";
                    std::cerr << "请将 dblp.xml 放在当前目录，或通过命令行参数指定路径，例如:\n";
                    std::cerr << "  ./dblp_extreme /path/to/dblp.xml\n";
                    return 1;
                }
            } catch (const std::exception& ex) {
                std::cerr << "检查数据文件路径时发生异常: " << ex.what() << '\n';
                return 1;
            }

            const auto t_parse_begin = std::chrono::steady_clock::now();
            ExtremeParser parser(0);
            std::vector<std::unique_ptr<LocalIndex>> locals = parser.parse_file(xml_path.c_str());
            const auto t_parse_done = std::chrono::steady_clock::now();
            if (locals.empty()) {
                std::cerr << "错误: 解析未产生任何索引分片（文件为空或无法读取）。\n";
                return 1;
            }

            engine.merge_local_indexes(std::move(locals));
            const auto t_merge_done = std::chrono::steady_clock::now();

            const double parse_sec =
                std::chrono::duration<double>(t_parse_done - t_parse_begin).count();
            const double merge_sec =
                std::chrono::duration<double>(t_merge_done - t_parse_done).count();
            const double total_sec =
                std::chrono::duration<double>(t_merge_done - t_parse_begin).count();
            std::cout << "\n[计时] XML 解析耗时(parse_file): " << parse_sec << " 秒\n";
            std::cout << "[计时] 全局归并耗时(merge_local_indexes): " << merge_sec << " 秒\n";
            std::cout << "[计时] 总耗时(parse + merge + save segment): " << total_sec << " 秒\n";
        }
        std::cout << "已载入文档数: " << engine.document_count() << "，平均标题词数(avgdl): " << engine.average_doc_length() << "\n";
    } catch (const std::exception& ex) {
        std::cerr << "解析或归并阶段发生异常: " << ex.what() << '\n';
        return 1;
    }

    if (benchmark_mode) {
        run_query_benchmark(engine, std::cout);
        return 0;
    }

    for (;;) {
        print_menu();
        const int choice = read_menu_choice();
        if (choice == 0) {
            std::cout << "感谢使用，再见。\n";
            return 0;
        }

        if (choice == 1) {
            const std::string q = read_user_line("请输入作者检索串（支持拼写容错，可用 fuzzy:on/off、fuzzy:1/2）: ");
            run_search_timed(engine, [&]() { engine.search_by_author(q, std::cout); });
            continue;
        }
        if (choice == 2) {
            const std::string q = read_user_line("请输入标题检索串: ");
            run_search_timed(engine, [&]() { engine.search_by_title(q, std::cout); });
            continue;
        }
        if (choice == 3) {
            const std::string q = read_user_line("请输入目标作者: ");
            run_search_timed(engine, [&]() { engine.search_collaborators(q, std::cout); });
            continue;
        }
        if (choice == 4) {
            const std::string q = build_f5_search_query();
            if (q.empty()) {
                std::cout << "输入为空，已取消搜索。\n";
                continue;
            }
            F5SearchProfile prof;
            F5SearchOptions opts;
            opts.emit_results = true;
            opts.profile = &prof;
            const auto t0 = std::chrono::steady_clock::now();
            engine.search_bm25(q, std::cout, opts);
            const auto t1 = std::chrono::steady_clock::now();
            prof.print(std::cout);
            const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            std::cout << "[计时] 本次检索墙钟耗时: " << wall_ms << " ms\n";
            continue;
        }
        if (choice == 5) {
            (void)read_user_line("（预留）可输入统计范围说明，直接回车跳过: ");
            const auto t0 = std::chrono::steady_clock::now();
            engine.execute_f3_author_stats();
            const auto t1 = std::chrono::steady_clock::now();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            std::cout << "[计时] 本次调用耗时: " << ms << " ms\n";
            continue;
        }
        if (choice == 6) {
            (void)read_user_line("（预留）可输入年份或关键词，直接回车跳过: ");
            const auto t0 = std::chrono::steady_clock::now();
            engine.execute_f4_conference_analytics();
            const auto t1 = std::chrono::steady_clock::now();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            std::cout << "[计时] 本次调用耗时: " << ms << " ms\n";
            continue;
        }
        if (choice == 7) {
            const auto t0 = std::chrono::steady_clock::now();
            engine.execute_f6_global_ranking();
            const auto t1 = std::chrono::steady_clock::now();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            std::cout << "[计时] 本次调用耗时: " << ms << " ms\n";
            continue;
        }
        if (choice == 8) {
            (void)read_user_line("（预留）可输入导出路径或格式说明，直接回车跳过: ");
            const auto t0 = std::chrono::steady_clock::now();
            engine.execute_f7_export_report();
            const auto t1 = std::chrono::steady_clock::now();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            std::cout << "[计时] 本次调用耗时: " << ms << " ms\n";
            continue;
        }
        if (choice == 9) {
            const auto t0 = std::chrono::steady_clock::now();
            run_query_benchmark(engine, std::cout);
            const auto t1 = std::chrono::steady_clock::now();
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            std::cout << "\n[计时] 评测集总墙钟耗时: " << ms << " ms\n";
            continue;
        }

        std::cout << "无效选项，请重新输入。\n";
    }
}
