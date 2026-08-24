#include "jade/Deploy.hpp"

#include <filesystem>
#include <iostream>
#include <string>

namespace {

std::string json_string(const std::string& value) {
    std::string out = "\"";
    for (const unsigned char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    static const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out += hex[c >> 4];
                    out += hex[c & 15];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
    return out;
}

std::string path_text(const std::filesystem::path& path) {
#ifdef _WIN32
    return path.u8string();
#else
    return path.string();
#endif
}

void usage() {
    std::cerr
        << "usage:\n"
        << "  deploy_cli backup-name <target-name> <game-code>\n"
        << "  deploy_cli deploy <built> <game-dir> <target-name> <game-code> "
           "<backup-dir> [backup: 0|1]\n"
        << "  deploy_cli restore <game-dir> <target-name> <game-code> "
           "<backup-dir>\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "backup-name") {
        if (argc != 4) {
            usage();
            return 2;
        }
        std::cout << json_string(jade::deploy::backup_name(argv[2], argv[3]))
                  << '\n';
        return 0;
    }

    if (argc >= 2 && std::string(argv[1]) == "deploy") {
        if (argc != 7 && argc != 8) {
            usage();
            return 2;
        }
        jade::deploy::Options options;
        options.backup_dir = std::filesystem::u8path(argv[6]);
        options.backup = argc == 7 || std::string(argv[7]) != "0";
        const jade::deploy::DeployResult result = jade::deploy::deploy(
            std::filesystem::u8path(argv[2]), std::filesystem::u8path(argv[3]),
            argv[4], argv[5], options);
        std::cout << "{\"ok\":" << (result.ok() ? "true" : "false")
                  << ",\"error\":"
                  << json_string(jade::deploy::error_name(result.error))
                  << ",\"message\":" << json_string(result.message)
                  << ",\"target\":";
        if (result.target.empty())
            std::cout << "null";
        else
            std::cout << json_string(path_text(result.target));
        std::cout << ",\"backup\":";
        if (!result.has_backup)
            std::cout << "null";
        else
            std::cout << json_string(path_text(result.backup));
        std::cout << ",\"backed_up_now\":"
                  << (result.backed_up_now ? "true" : "false") << "}\n";
        return 0;
    }

    if (argc >= 2 && std::string(argv[1]) == "restore") {
        if (argc != 6) {
            usage();
            return 2;
        }
        const jade::deploy::RestoreResult result = jade::deploy::restore_stock(
            std::filesystem::u8path(argv[2]), argv[3], argv[4],
            std::filesystem::u8path(argv[5]));
        std::cout << "{\"ok\":" << (result.ok() ? "true" : "false")
                  << ",\"error\":"
                  << json_string(jade::deploy::error_name(result.error))
                  << ",\"message\":" << json_string(result.message)
                  << ",\"target\":";
        if (result.target.empty())
            std::cout << "null";
        else
            std::cout << json_string(path_text(result.target));
        std::cout << "}\n";
        return 0;
    }

    usage();
    return 2;
}
