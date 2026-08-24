// project_cli — native mod-project build.
//   project_cli <jmod_dir> <base.bf> <out.bf|-> [--dry-run]
//   project_cli <jmod_dir> <base.bf> <built.bf> --verify-semantic
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "jade/Project.hpp"
#include "jade/Utf8Args.hpp"

using namespace jade::project;

int main(int argc, char** argv) {
    jade::Utf8Args command_line(argc, argv);
    argc = command_line.argc();
    argv = command_line.argv();
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: project_cli <jmod_dir> <base.bf> <out.bf|-> [--dry-run]\n");
        return 2;
    }
    bool dry = false, spins = false, validate_only = false;
    bool verify_semantic = false, trace_progress = false;
    bool print_report = false;
    bool validate_live = false;
    bool conflicts_only = false;
    bool summaries_only = false;
    bool roundtrip_only = false;
    bool load_result_only = false;
    BuildFaultInjection build_fault;
    std::string jtmod_out;
    JtmodExportOptions jtmod_options;
    jtmod_options.created_iso = "2026-01-01T00:00:00Z";
    jtmod_options.validate = false;
    for (int i = 4; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dry-run") == 0) dry = true;
        else if (std::strcmp(argv[i], "--validate-spins") == 0) spins = true;
        else if (std::strcmp(argv[i], "--validate") == 0) validate_only = true;
        else if (std::strcmp(argv[i], "--validate-live") == 0) {
            validate_only = true;
            validate_live = true;
        }
        else if (std::strcmp(argv[i], "--verify-semantic") == 0)
            verify_semantic = true;
        else if (std::strcmp(argv[i], "--trace-progress") == 0)
            trace_progress = true;
        else if (std::strcmp(argv[i], "--print-report") == 0)
            print_report = true;
        else if (std::strcmp(argv[i], "--conflicts") == 0)
            conflicts_only = true;
        else if (std::strcmp(argv[i], "--summaries") == 0)
            summaries_only = true;
        else if (std::strcmp(argv[i], "--roundtrip") == 0)
            roundtrip_only = true;
        else if (std::strcmp(argv[i], "--load-result") == 0)
            load_result_only = true;
        else if (std::strcmp(argv[i], "--test-fail-write-entry") == 0 &&
                 i + 1 < argc)
            build_fault.fail_write_entry_at =
                static_cast<size_t>(std::strtoull(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--export-jtmod") == 0 && i + 1 < argc)
            jtmod_out = argv[++i];
        else if (std::strcmp(argv[i], "--jtmod-title") == 0 && i + 1 < argc)
            jtmod_options.title = argv[++i];
        else if (std::strcmp(argv[i], "--jtmod-author") == 0 && i + 1 < argc)
            jtmod_options.author = argv[++i];
        else if (std::strcmp(argv[i], "--jtmod-version") == 0 && i + 1 < argc)
            jtmod_options.version = argv[++i];
        else if (std::strcmp(argv[i], "--jtmod-description") == 0
                 && i + 1 < argc)
            jtmod_options.description = argv[++i];
        else if (std::strcmp(argv[i], "--jtmod-image") == 0 && i + 1 < argc)
            jtmod_options.image_path = argv[++i];
        else if (std::strcmp(argv[i], "--jtmod-created") == 0 && i + 1 < argc)
            jtmod_options.created_iso = argv[++i];
        else if (std::strcmp(argv[i], "--jtmod-validate") == 0)
            jtmod_options.validate = true;
    }
    ModProject p = load_project(argv[1]);
    if (load_result_only) {
        jade::json::Value output = jade::json::make_obj();
        output.obj["ok"] = jade::json::make_bool(p.ok);
        if (p.ok) {
            output.obj["project"] = project_to_dict(p);
        } else {
            output.obj["error_type"] = jade::json::make_str(p.error_type);
            output.obj["message"] = jade::json::make_str(p.error);
        }
        std::printf("LOAD_RESULT %s\n", jade::json::dump(output).c_str());
        return 0;
    }
    if (!p.ok) { std::printf("ERROR %s\n", p.error.c_str()); return 0; }
    std::printf("PROJECT name=%s game=%s ops=%zu\n", p.name.c_str(),
                p.base.game.c_str(), p.operations.size());
    if (conflicts_only) {
        jade::json::Value output = jade::json::make_arr();
        for (const ProjectConflict& conflict : project_conflicts(p)) {
            jade::json::Value item = jade::json::make_obj();
            jade::json::Value signature = jade::json::make_arr();
            signature.arr.push_back(
                jade::json::make_str(conflict.signature.kind));
            for (const ConflictValue& value : conflict.signature.values) {
                signature.arr.push_back(
                    value.type == ConflictValue::Type::String
                        ? jade::json::make_str(value.string)
                        : jade::json::make_num(double(value.integer)));
            }
            jade::json::Value ids = jade::json::make_arr();
            for (const std::string& id : conflict.op_ids)
                ids.arr.push_back(jade::json::make_str(id));
            item.obj["signature"] = std::move(signature);
            item.obj["op_ids"] = std::move(ids);
            item.obj["formatted"] = jade::json::make_str(
                format_conflict_signature(conflict.signature));
            output.arr.push_back(std::move(item));
        }
        std::printf("CONFLICTS %s\n", jade::json::dump(output).c_str());
        return 0;
    }
    if (summaries_only) {
        jade::json::Value output = jade::json::make_arr();
        for (const jade::json::Value& operation : p.operations)
            output.arr.push_back(jade::json::make_str(
                operation_target_summary(operation)));
        std::printf("SUMMARIES %s\n", jade::json::dump(output).c_str());
        return 0;
    }
    if (roundtrip_only) {
        std::printf("ROUNDTRIP %s\n",
                    jade::json::dump(project_to_dict(p)).c_str());
        return 0;
    }
    if (validate_only) {
        std::vector<std::string> vlog;
        auto vi = validate_project(p, argv[2], vlog, validate_live);
        for (const std::string& l : vlog) std::printf("LOG %s\n", l.c_str());
        size_t errs = 0, warns = 0;
        for (const BuildIssue& i : vi) {
            std::string line = "[" + i.level + "]";
            if (!i.op_id.empty()) line += " " + i.op_id;
            line += " " + i.message;
            std::printf("ISSUE %s\n", line.c_str());
            (i.level == "error" ? errs : warns) += 1;
        }
        std::printf("VALIDATE errors=%zu warnings=%zu\n", errs, warns);
        return 0;
    }
    if (verify_semantic) {
        std::vector<std::string> vlog;
        std::vector<BuildIssue> vi =
            verify_replaced_textures(p, argv[3], vlog);
        std::vector<BuildIssue> ai = verify_added_assets(
            p, argv[2], argv[3], vlog);
        vi.insert(vi.end(), ai.begin(), ai.end());
        for (const std::string& l : vlog) std::printf("LOG %s\n", l.c_str());
        size_t errors = 0, warnings = 0;
        for (const BuildIssue& i : vi) {
            std::string line = "[" + i.level + "]";
            if (!i.op_id.empty()) line += " " + i.op_id;
            line += " " + i.message;
            std::printf("ISSUE %s\n", line.c_str());
            if (i.level == "error") ++errors;
            else if (i.level == "warning") ++warnings;
        }
        std::printf("SEMANTIC errors=%zu warnings=%zu\n", errors, warnings);
        return 0;
    }
    if (!jtmod_out.empty()) {
        JtmodExportResult jr = export_jtmod(p, argv[2], jtmod_out,
                                            jtmod_options);
        for (const std::string& l : jr.log) std::printf("LOG %s\n", l.c_str());
        for (const BuildIssue& i : jr.issues)
            std::printf("ISSUE [%s] %s\n", i.level.c_str(), i.message.c_str());
        std::printf("JTMOD ok=%d bytes=%zu sub=%u whole=%u new=%u minted=%zu\n",
                    jr.ok ? 1 : 0, jr.blob.size(), jr.bins_subentry,
                    jr.bins_wholebin, jr.bins_new, jr.minted_keys.size());
        return 0;
    }
    std::string out = std::strcmp(argv[3], "-") == 0 ? "" : argv[3];
    BuildProgressFn progress;
    if (trace_progress) {
        progress = [](uint64_t done, uint64_t total,
                      const std::string& phase) {
            std::printf("PROGRESS %llu %llu %s\n",
                        (unsigned long long)done,
                        (unsigned long long)total, phase.c_str());
        };
    }
    const BuildFaultInjection* fault =
        build_fault.fail_write_entry_at ? &build_fault : nullptr;
    BuildResult r = build_project(p, argv[2], out, dry, progress, fault);
    if (print_report)
        std::printf("REPORT_BEGIN\n%s\nREPORT_END\n", r.report.c_str());
    for (const std::string& l : r.log) std::printf("LOG %s\n", l.c_str());
    for (const BuildIssue& i : r.issues) {
        std::string line = "[" + i.level + "]";
        if (!i.op_id.empty()) line += " " + i.op_id;
        line += " " + i.message;
        std::printf("ISSUE %s\n", line.c_str());
    }
    if (spins && r.ok && !dry) {
        std::vector<std::string> slog;
        auto sw = validate_spins(argv[2], out.empty() ? r.output_path : out,
                                 r.modified_keys, slog);
        for (const std::string& l : slog) std::printf("LOG %s\n", l.c_str());
        for (const BuildIssue& i : sw)
            std::printf("ISSUE [%s] %s\n", i.level.c_str(), i.message.c_str());
        std::printf("SPINCHECK warnings=%zu\n", sw.size());
    }
    std::printf("BUILD ok=%d changed=%u appended=%u bytes=%llu output=%s\n",
                r.ok ? 1 : 0,
                r.entries_changed, r.entries_appended,
                (unsigned long long)r.bytes_appended, r.output_path.c_str());
    return 0;
}
