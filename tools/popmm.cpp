// popmm — PoP Mod Manager headless CLI (the engine entry point the GUI wraps).
//
//   popmm info   <mod.jtmod>
//   popmm verify <base.bf> <mod.jtmod> [mod.jtmod ...]
//   popmm apply  [--allow-conflicts] <base.bf> <out.bf> <mod.jtmod> [...]
#include <cstdio>
#include <string>
#include <vector>

#include "jade/JtmodEngine.hpp"

using namespace jade::jtmod;

static int usage() {
    std::fprintf(stderr,
        "usage:\n"
        "  popmm info   <mod.jtmod>\n"
        "  popmm verify <base.bf> <mod.jtmod> [mod.jtmod ...]\n"
        "  popmm apply  [--allow-conflicts] <base.bf> <out.bf> <mod.jtmod> [...]\n");
    return 2;
}

int main(int argc, char** argv) {
    if (argc < 2) return usage();
    std::string cmd = argv[1];
    try {
        if (cmd == "info") {
            if (argc < 3) return usage();
            Mod m = load_mod(argv[2]);
            std::printf("name: %s\nauthor: %s\nversion: %s\ngame: %s\n"
                        "archive: %s\nbase_size: %llu\nbase_sha256: %s\n"
                        "minted_keys: %zu\nbins: %zu\n",
                        m.data.name.c_str(), m.data.author.c_str(),
                        m.data.version_text.c_str(), m.data.game.c_str(),
                        m.data.archive_name.c_str(),
                        (unsigned long long)m.data.base_size,
                        m.data.base_sha256.c_str(), m.data.minted_keys.size(),
                        m.data.bins.size());
            return 0;
        }
        if (cmd == "verify") {
            if (argc < 4) return usage();
            std::string base = argv[2];
            std::vector<Mod> mods;
            for (int i = 3; i < argc; ++i) mods.push_back(load_mod(argv[i]));
            std::vector<std::string> problems = precheck(mods, base);
            std::vector<Conflict> conflicts = detect_conflicts(mods);
            for (const std::string& p : problems) std::printf("PROBLEM: %s\n", p.c_str());
            for (const Conflict& c : conflicts) std::printf("CONFLICT: %s\n", c.str().c_str());
            if (problems.empty() && conflicts.empty()) {
                std::printf("OK: %zu mod(s) apply cleanly\n", mods.size());
                return 0;
            }
            return 1;
        }
        if (cmd == "apply") {
            int i = 2;
            bool allow = false, force = false;
            for (; i < argc; ++i) {
                std::string a = argv[i];
                if (a == "--allow-conflicts") allow = true;
                else if (a == "--force-base") force = true;
                else break;
            }
            if (argc - i < 3) return usage();
            std::string base = argv[i++], out = argv[i++];
            std::vector<Mod> mods;
            for (; i < argc; ++i) mods.push_back(load_mod(argv[i]));
            ApplyResult r = apply(base, mods, out, allow, force);
            std::printf("%s\n", r.report.c_str());
            for (const Conflict& c : r.conflicts) std::printf("CONFLICT: %s\n", c.str().c_str());
            for (const std::string& is : r.issues) std::printf("ISSUE: %s\n", is.c_str());
            std::printf(r.ok ? "OK\n" : "FAILED\n");
            return r.ok ? 0 : 1;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 3;
    }
    return usage();
}
