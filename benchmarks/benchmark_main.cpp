#include <benchmark/benchmark.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

static bool has_flag(int argc, char** argv, const char* prefix) {
    for (int i = 0; i < argc; ++i) {
        const char* a = argv[i];
        if (a == nullptr) {
            continue;
        }
        if (std::strncmp(a, prefix, std::strlen(prefix)) == 0) {
            return true;
        }
    }
    return false;
}

static bool repetitions_gt_one(int argc, char** argv) {
    const char* key = "--benchmark_repetitions";
    for (int i = 0; i < argc; ++i) {
        const std::string a = argv[i] ? std::string(argv[i]) : std::string();
        if (a.rfind(key, 0) != 0) {
            continue;
        }
        if (a == key && i + 1 < argc) {
            return std::atoi(argv[i + 1]) > 1;
        }
        const auto pos = a.find('=');
        if (pos != std::string::npos) {
            return std::atoi(a.c_str() + pos + 1) > 1;
        }
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> extra;

    const bool has_reps = has_flag(argc, argv, "--benchmark_repetitions");
    const bool need_agg = has_reps && repetitions_gt_one(argc, argv) &&
                          !has_flag(argc, argv, "--benchmark_report_aggregates_only");
    if (need_agg) {
        extra.emplace_back("--benchmark_report_aggregates_only=true");
    }

    std::vector<char*> argv2;
    argv2.reserve(static_cast<std::size_t>(argc) + extra.size());
    for (int i = 0; i < argc; ++i) {
        argv2.push_back(argv[i]);
    }
    for (auto& s : extra) {
        argv2.push_back(s.data());
    }

    int argc2 = static_cast<int>(argv2.size());
    benchmark::Initialize(&argc2, argv2.data());
    if (benchmark::ReportUnrecognizedArguments(argc2, argv2.data())) {
        return 1;
    }
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
