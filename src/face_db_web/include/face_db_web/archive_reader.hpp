#pragma once

#ifndef FACE_DB_WEB_ARCHIVE_READER_HPP
#define FACE_DB_WEB_ARCHIVE_READER_HPP

/**
 * In-memory reader for the archive formats the bulk-import UI accepts.
 *
 * Supported: .zip (stored + deflate), .tar and .tar.gz / .tgz.
 * Everything is decoded into memory because the entries are fed straight to
 * the detector — no temp directory to clean up and nothing left behind if the
 * import fails half way.
 *
 * The limits exist because an archive is attacker/user-controlled input: a
 * crafted "zip bomb" would otherwise expand until the process is OOM-killed.
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace face_db_web {

enum class ArchiveKind {
    Unknown,
    Zip,
    Tar,
    Gzip,   // .tar.gz / .tgz (a gzip stream wrapping a tar)
};

struct ArchiveLimits {
    std::size_t max_entries      = 4000;
    std::size_t max_total_bytes  = 512ull * 1024 * 1024;   // decompressed total
    std::size_t max_entry_bytes  = 64ull * 1024 * 1024;    // one image
};

struct ArchiveEntry {
    std::string          path;       // as stored inside the archive
    std::string          basename;   // path without directories
    std::vector<uint8_t> data;
};

/** Detect by magic bytes first (a renamed archive is still an archive). */
ArchiveKind detectArchiveKind(const std::string& filename,
                              const std::uint8_t* data, std::size_t size);

/**
 * Decode the archive at [data, data+size) into `out`.
 *
 * Takes a raw pointer rather than a container on purpose: the caller already
 * holds the uploaded bytes (request body / multipart part), and copying a
 * multi-hundred-megabyte archive just to pass it here would double the peak
 * memory of an import.
 *
 * Entries that expand beyond the limits stop the whole read with an error,
 * rather than silently truncating.
 */
bool readArchive(const std::uint8_t* data, std::size_t size,
                 const std::string& filename,
                 std::vector<ArchiveEntry>& out,
                 std::string& error,
                 const ArchiveLimits& limits = ArchiveLimits{});

/** Convenience overload for callers that already have a buffer. */
inline bool readArchive(const std::vector<std::uint8_t>& data,
                        const std::string& filename,
                        std::vector<ArchiveEntry>& out,
                        std::string& error,
                        const ArchiveLimits& limits = ArchiveLimits{}) {
    return readArchive(data.data(), data.size(), filename, out, error, limits);
}

}  // namespace face_db_web

#endif  // FACE_DB_WEB_ARCHIVE_READER_HPP
