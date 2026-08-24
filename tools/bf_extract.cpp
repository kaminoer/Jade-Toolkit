// bf_extract -- read and decompress one BigFile entry without mutating it.
//
//   bf_extract <file.bf> <entry-index> <output>
//   bf_extract <file.bf> --batch <index-list.txt> <output-directory>
//
// This deliberately small diagnostic boundary lets Python parity harnesses
// use the verified bundled miniLZO decoder when python-lzo is unavailable.
// All archive parsing and product-level validation still runs independently
// in the Python toolkit and the native implementation.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "jade/BigFile.hpp"
#include "jade/Compression.hpp"
#include "jade/Utf8Args.hpp"

namespace {

bool parse_index(const char* text, uint32_t& index) {
    errno = 0;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text, &end, 0);
    if (errno == ERANGE || !end || *end != '\0' ||
        parsed > std::numeric_limits<uint32_t>::max())
        return false;
    index = static_cast<uint32_t>(parsed);
    return true;
}

bool extract_one(const jade::BigFile& bf, uint32_t index,
                 const std::filesystem::path& output,
                 size_t max_output = 0) {
    const auto found = bf.files.find(index);
    if (found == bf.files.end() || found->second.name.empty() ||
        found->second.key == jade::INVALID_KEY) {
        std::fprintf(stderr, "bf_extract: entry %u is not active\n", index);
        return false;
    }

    const std::vector<uint8_t> raw = bf.read_data(index);
    const jade::LzoResult result = jade::decompress_lzo(raw, max_output);
    if (!result.ok) {
        std::fprintf(stderr, "bf_extract: entry %u did not decompress\n", index);
        return false;
    }

    std::ofstream stream(output, std::ios::binary | std::ios::trunc);
    if (!stream) {
        std::fprintf(stderr, "bf_extract: cannot write %s\n",
                     output.string().c_str());
        return false;
    }
    stream.write(reinterpret_cast<const char*>(result.data.data()),
                 static_cast<std::streamsize>(result.data.size()));
    if (!stream) {
        std::fprintf(stderr, "bf_extract: write failed for %s\n",
                     output.string().c_str());
        return false;
    }

    std::printf("EXTRACT index=%u key=%08x raw=%zu decoded=%zu total=%u\n",
                index, found->second.key, raw.size(), result.data.size(),
                result.total_dec);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    jade::Utf8Args command_line(argc, argv);
    argc = command_line.argc();
    argv = command_line.argv();
    const bool batch = argc == 5 && std::string(argv[2]) == "--batch";
    if (argc != 4 && !batch) {
        std::fprintf(stderr,
                     "usage: bf_extract <file.bf> <entry-index> <output>\n"
                     "       bf_extract <file.bf> --batch <index-list.txt> "
                     "<output-directory>\n");
        return 2;
    }

    try {
        jade::BigFile bf;
        bf.open(argv[1]);
        if (!batch) {
            uint32_t index = 0;
            if (!parse_index(argv[2], index)) {
                std::fprintf(stderr, "bf_extract: invalid entry index %s\n",
                             argv[2]);
                return 2;
            }
            return extract_one(bf, index, argv[3]) ? 0 : 1;
        }

        std::ifstream list(argv[3]);
        if (!list) {
            std::fprintf(stderr, "bf_extract: cannot read index list %s\n",
                         argv[3]);
            return 2;
        }
        const std::filesystem::path directory(argv[4]);
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) {
            std::fprintf(stderr, "bf_extract: cannot create %s: %s\n",
                         argv[4], error.message().c_str());
            return 2;
        }
        std::string token;
        size_t count = 0;
        size_t failed = 0;
        while (list >> token) {
            uint32_t index = 0;
            if (!parse_index(token.c_str(), index)) {
                std::fprintf(stderr, "bf_extract: invalid entry index %s\n",
                             token.c_str());
                return 2;
            }
            if (!extract_one(bf, index,
                             directory / (std::to_string(index) + ".bin"),
                             256)) {
                ++failed;  // Python's true-wow indexer skips bad entries too.
                continue;
            }
            ++count;
        }
        std::printf("BATCH count=%zu failed=%zu\n", count, failed);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "bf_extract: error: %s\n", error.what());
        return 1;
    }
}
