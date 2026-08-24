// zonetx_cli — cross-game zone transplant.
//   zonetx_cli <donor.bf> <target.bf> <out.bf> <zone_name> [exclude_hex,...]
//              [--log] [--details] [--preindexed]
// <out.bf> must already be a copy of the target (harnesses copy it). Prints
// the dep classification, the plan summary, and the execute stats.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_set>

#include "jade/BigFile.hpp"
#include "jade/Keys.hpp"
#include "jade/ZoneTransplant.hpp"
#include "jade/Utf8Args.hpp"

using namespace jade;

int main(int argc, char** argv) {
    jade::Utf8Args command_line(argc, argv);
    argc = command_line.argc();
    argv = command_line.argv();
    if (argc < 5) {
        std::fprintf(stderr,
            "usage: zonetx_cli <donor.bf> <target.bf> <out.bf> <zone_name> "
            "[excl,...] [--log] [--details] [--preindexed]\n");
        return 2;
    }
    std::unordered_set<uint32_t> exclude;
    bool print_log = false;
    bool print_details = false;
    bool preindexed = false;
    for (int argi = 5; argi < argc; ++argi) {
        if (std::strcmp(argv[argi], "--log") == 0) {
            print_log = true;
            continue;
        }
        if (std::strcmp(argv[argi], "--details") == 0) {
            print_details = true;
            continue;
        }
        if (std::strcmp(argv[argi], "--preindexed") == 0) {
            preindexed = true;
            continue;
        }
        const char* p = argv[argi];
        while (*p) {
            exclude.insert(uint32_t(std::strtoul(p, nullptr, 16)));
            const char* c = std::strchr(p, ',');
            if (!c) break;
            p = c + 1;
        }
    }

    BigFile donor, target;
    try {
        donor.open(argv[1]);
        target.open(argv[2]);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "zonetx_cli: %s\n", e.what());
        return 1;
    }

    zonetx::TrueWowIndex donor_index;
    zonetx::TrueWowIndex target_index;
    if (preindexed) {
        donor_index = build_true_wow_index(donor);
        target_index = build_true_wow_index(target);
    }
    zonetx::TransplantPlan plan = zonetx::plan_transplant(
        donor, target, argv[4], exclude,
        preindexed ? &donor_index : nullptr,
        preindexed ? &target_index : nullptr);
    if (!plan.ok) { std::printf("ERROR %s\n", plan.error.c_str()); return 0; }

    for (const zonetx::DepEntry& d : plan.deps)
        std::printf("DEP %08x %s%s len=%u %s\n", d.dep_key,
                    zonetx::dep_status_name(d.status),
                    d.is_self_ref ? " self" : "", d.length,
                    d.name.empty() ? "-" : d.name.c_str());
    for (const auto& c : plan.collisions)
        std::printf("COLLISION %08x %s\n", c.first, c.second.c_str());
    std::printf("PLAN zone=%s ikey=%08x wolkey=%08x copies=%zu collisions=%zu "
                "excluded=%u wolrebuilt=%d\n",
                plan.zone_name.c_str(), plan.zone_internal_key, plan.wol_bf_key,
                plan.copy_items.size(), plan.collisions.size(),
                plan.excluded_count, plan.has_wol_dec ? 1 : 0);
    if (print_details)
        std::printf("SUMMARY present=%zu copy=%zu unresolved=%zu "
                    "total_copy_bytes=%llu\n",
                    plan.present().size(), plan.to_copy().size(),
                    plan.unresolved().size(),
                    (unsigned long long)plan.total_copy_bytes());

    BigFileLogFn log;
    if (print_log)
        log = [](const std::string& line) {
            std::printf("LOG %s\n", line.c_str());
        };
    zonetx::TransplantStats st =
        zonetx::execute_transplant(donor, argv[3], plan, -1, log);
    if (!st.ok) { std::printf("ERROR %s\n", st.error.c_str()); return 0; }
    std::printf("EXEC added=%u bytes=%llu skipped=%u grs=%d\n", st.added,
                (unsigned long long)st.added_bytes, st.skipped, st.size_grs_rows);
    if (print_details) {
        std::printf("STATS collisions=%zu added_keys=", st.collisions.size());
        for (size_t i = 0; i < st.added_keys.size(); ++i)
            std::printf("%s%08x", i ? "," : "", st.added_keys[i]);
        std::printf(" excluded=%u\n", st.excluded);
    }
    return 0;
}
