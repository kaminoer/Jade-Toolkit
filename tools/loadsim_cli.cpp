// loadsim_cli — static zone-load simulation (spin detector).
//   loadsim_cli <bf> <wol_hex[,wol_hex...]>
// Builds the sub-key provider index once, then prints one report per WOL.
// Referrers print sorted (they're set-shaped in the Python).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/LoadSim.hpp"

using namespace jade;

static void print_log(const std::string& line) {
    std::printf("%s\n", line.c_str());
}

static void print_index(const loadsim::KeyIndex& index, bool details) {
    std::printf("INDEX keys=%zu\n", index.items.size());
    if (!details) return;
    for (const auto& item : index.items) {
        std::printf("KEY %08x prov=", item.first);
        for (size_t i = 0; i < item.second.size(); ++i) {
            if (i) std::printf(";");
            std::printf("%08x:%s", item.second[i].first,
                        item.second[i].second.c_str());
        }
        std::printf("\n");
    }
}

static void print_report(const loadsim::SimReport& report) {
    std::printf("SIM wol=%08X ndeps=%u loaded=%u closure=%zu missing=%zu "
                "unresolved=%zu\n",
                report.wol_key, report.n_deps, report.deps_loaded,
                report.closure_size, report.missing_dep_bins.size(),
                report.unresolved.size());
    for (const auto& missing : report.missing_dep_bins)
        std::printf("MISS %08x %08x %d\n", std::get<0>(missing),
                    std::get<1>(missing), std::get<2>(missing) ? 1 : 0);
    for (const loadsim::Unresolved& unresolved : report.unresolved) {
        std::string providers;
        for (const auto& provider : unresolved.providers) {
            if (!providers.empty()) providers += ";";
            char buffer[16];
            std::snprintf(buffer, sizeof buffer, "%08x:", provider.first);
            providers += buffer;
            providers += provider.second;
        }
        auto refs = unresolved.referrers;
        std::sort(refs.begin(), refs.end());
        std::string referrers;
        for (const auto& ref : refs) {
            if (!referrers.empty()) referrers += ";";
            char buffer[32];
            std::snprintf(buffer, sizeof buffer, "|%08X|%u",
                          std::get<1>(ref), std::get<2>(ref));
            referrers += std::get<0>(ref);
            referrers += buffer;
        }
        std::printf("UNRES %08x prov=%s refs=%s\n", unresolved.ref,
                    providers.c_str(), referrers.c_str());
    }
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: loadsim_cli <bf> <wol_hex[,wol_hex...]>\n");
        return 2;
    }
    BigFile bf;
    try { bf.open(argv[1]); } catch (const std::exception& e) {
        std::fprintf(stderr, "loadsim_cli: %s\n", e.what());
        return 1;
    }
    if (std::strcmp(argv[2], "index") == 0) {
        if (argc > 4 || (argc == 4 &&
            std::strcmp(argv[3], "--cache") != 0 &&
            std::strcmp(argv[3], "--no-cache") != 0)) {
            std::fprintf(stderr,
                         "usage: loadsim_cli <bf> index [--cache|--no-cache]\n");
            return 2;
        }
        const bool cache = argc < 4 || std::strcmp(argv[3], "--no-cache") != 0;
        try {
            const loadsim::KeyIndex index = loadsim::build_key_index(
                bf, argv[1], cache, print_log);
            print_index(index, true);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "loadsim_cli: %s\n", error.what());
            return 1;
        }
        return 0;
    }
    if (std::strcmp(argv[2], "auto") == 0) {
        if (argc < 4 || argc > 5 || (argc == 5 &&
            std::strcmp(argv[4], "--cache") != 0 &&
            std::strcmp(argv[4], "--no-cache") != 0)) {
            std::fprintf(stderr,
                "usage: loadsim_cli <bf> auto <wol_hex> [--cache|--no-cache]\n");
            return 2;
        }
        const bool cache = argc < 5 || std::strcmp(argv[4], "--no-cache") != 0;
        try {
            const uint32_t wol = uint32_t(std::strtoul(argv[3], nullptr, 16));
            const loadsim::SimReport report = loadsim::simulate_zone(
                bf, argv[1], wol, nullptr, cache, print_log);
            print_report(report);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "loadsim_cli: %s\n", error.what());
            return 1;
        }
        return 0;
    }
    std::vector<uint32_t> wols;
    for (const char* p = argv[2]; *p;) {
        wols.push_back(uint32_t(std::strtoul(p, nullptr, 16)));
        const char* c = std::strchr(p, ',');
        if (!c) break;
        p = c + 1;
    }

    loadsim::KeyIndex idx = loadsim::build_key_index(bf);
    print_index(idx, false);

    for (uint32_t wol : wols) {
        print_report(loadsim::simulate_zone(bf, wol, idx));
    }
    return 0;
}
