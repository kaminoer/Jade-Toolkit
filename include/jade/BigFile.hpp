// BigFile.hpp — Jade BigFile (.bf) archive reader.
//
// Port of jade_explorer/core/bigfile.py (read side). Parses the FAT (File
// Allocation Table) to enumerate directories and files and provides
// random-access reading of individual entries.
//
// Phase 0 ports the read path (open + tree walk). The write path
// (write_entry / add_entry / flush_size_grs) lands in a later phase.
#pragma once

#include <cstdint>
#include <functional>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "jade/Reader.hpp"

namespace jade {

using BigFileLogFn = std::function<void(const std::string&)>;

// Constants from BIGdefs.h (kept identical to bigfile.py).
constexpr uint32_t INVALID_INDEX = 0xFFFFFFFFu;
constexpr uint32_t INVALID_KEY   = 0xFFFFFFFFu;
constexpr size_t   MAX_LEN_NAME  = 64;
constexpr size_t   HEADER_SIZE   = 44;
constexpr size_t   FATDES_SIZE   = 24;
constexpr size_t   FILE_ENTRY_SZ = 8;
constexpr size_t   DIR_ENTRY_SZ  = 84;
constexpr size_t   FILEEXT_OLD   = 84;
constexpr size_t   FILEEXT_NEW   = 88;
constexpr size_t   SECTOR_ALIGN  = 0x800;

// A file entry in the BigFile FAT.
struct BFFile {
    uint32_t index  = 0;
    uint32_t pos    = 0;
    uint32_t key    = INVALID_KEY;
    uint32_t length = 0;
    uint32_t prev   = INVALID_INDEX;
    uint32_t next   = INVALID_INDEX;
    uint32_t parent = INVALID_INDEX;
    uint32_t time   = 0;
    std::string name;
};

// A directory entry in the BigFile FAT.
struct BFDir {
    uint32_t index        = 0;
    uint32_t first_file   = INVALID_INDEX;
    uint32_t first_subdir = INVALID_INDEX;
    uint32_t prev         = INVALID_INDEX;
    uint32_t next         = INVALID_INDEX;
    uint32_t parent       = INVALID_INDEX;
    std::string name;
};

// One FAT descriptor (FATDes) record.
struct FatDesc {
    uint32_t max_file     = 0;
    uint32_t max_dir      = 0;
    uint32_t pos_fat      = 0;
    uint32_t next_pos_fat = 0;
    uint32_t first_index  = 0;
    uint32_t last_index   = 0;
};

class BigFile {
public:
    // Open and parse a BigFile archive. Throws std::runtime_error on a bad
    // magic / encrypted ('BUG') header / truncated file.
    void open(const std::string& path);

    // Raw data of a file entry by index (empty if missing or zero-length).
    std::vector<uint8_t> read_data(uint32_t idx) const;

    // Full '/'-joined path for a directory index.
    std::string dir_path(uint32_t idx) const;

    // Immediate child directory indices of a directory.
    std::vector<uint32_t> subdirs(uint32_t idx) const;

    // File indices directly under a directory.
    std::vector<uint32_t> dir_files(uint32_t idx) const;

    size_t active_file_count() const;
    size_t active_dir_count() const;

    // --- write path (port of bigfile.py write side) ---
    // All three take an std::fstream opened on `path` in
    // (in | out | binary) mode; they mutate the file on disk and the
    // in-memory FAT (`files`, `fat_list[0].max_file`, `max_file`). The caller
    // is responsible for already having LZO-compressed the payload (via
    // jade::compress_lzo) — these never compress.

    // Overwrite an existing entry: in place if `compressed` fits its slot, else
    // appended sector-aligned at EOF with its FAT pos + FileExt length
    // repointed. Queues a size.grs declared-length row (unless `fi` is
    // size.grs itself, or `skip_size_grs` is set — used for system tables the
    // engine loads RAW, e.g. WOLInfo, which must stay uncompressed and out of
    // size.grs). Returns the entry's new file position.
    uint32_t write_entry(std::fstream& f, const std::vector<uint8_t>& compressed,
                         BFFile& fi, bool skip_size_grs = false);

    // Create a brand-new file entry under `parent_dir_idx`, claiming the next
    // free over-provisioned FAT slot. Returns a reference to the new BFFile
    // (also inserted into `files`). Throws std::runtime_error on name too long,
    // key collision, full FAT, or unknown parent.
    BFFile& add_entry(std::fstream& f, const std::string& name, uint32_t key,
                      uint32_t parent_dir_idx,
                      const std::vector<uint8_t>& compressed,
                      const BigFileLogFn& log_fn = {});

    // Apply queued size.grs declared-length rows (from write_entry/add_entry):
    // update existing rows in place, fill trailing (0,0) slack for new keys,
    // grow + relocate the table when slack is exhausted. Returns rows changed.
    int flush_size_grs(std::fstream& f, const BigFileLogFn& log_fn = {});

    bool has_pending_size_grs() const { return !pending_size_grs_.empty(); }

    // --- parsed header fields (public, mirroring the Python attributes) ---
    std::string path;
    uint32_t version          = 0;
    uint32_t max_file         = 0;
    uint32_t max_dir          = 0;
    uint32_t max_key          = 0;
    uint32_t root             = 0;
    uint32_t first_free_file  = INVALID_INDEX;
    uint32_t first_free_dir   = INVALID_INDEX;
    uint32_t size_of_fat      = 0;
    uint32_t num_fat          = 0;
    uint32_t universe_key     = 0;
    bool     encrypted        = false;
    size_t   ext_size         = FILEEXT_OLD;

    std::vector<FatDesc> fat_list;
    std::map<uint32_t, BFFile> files;  // index -> file
    std::map<uint32_t, BFDir>  dirs;   // index -> dir

private:
    // Detect FileExt record size (84 vs 88) by validating the root dir entry.
    size_t detect_ext_size(std::ifstream& f, uint32_t pos_fat) const;

    // key -> LZO terminator offset, queued by write_entry/add_entry and applied
    // by flush_size_grs. INSERTION-ORDERED like bigfile.py's _pending_size_grs
    // dict — appended size.grs rows land in queue order, and a key-sorted map
    // produced byte-different archives on multi-entry flushes (zone transplant).
    std::vector<std::pair<uint32_t, long long>> pending_size_grs_;
    void queue_size_grs(uint32_t key, long long term) {
        for (auto& kv : pending_size_grs_)
            if (kv.first == key) { kv.second = term; return; }
        pending_size_grs_.push_back({key, term});
    }
};

}  // namespace jade
