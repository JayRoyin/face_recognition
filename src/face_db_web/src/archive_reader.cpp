#include "face_db_web/archive_reader.hpp"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace face_db_web {

namespace {

/**
 * Non-owning view over the uploaded archive.
 *
 * Exists so the helpers below can keep their `data.size()` / `&data[i]`
 * spelling while the reader never copies the (potentially huge) buffer.
 */
struct ByteView {
    const std::uint8_t* p = nullptr;
    std::size_t         n = 0;

    std::size_t size() const { return n; }
    const std::uint8_t* data() const { return p; }
    const std::uint8_t* begin() const { return p; }
    const std::uint8_t* end() const { return p + n; }
    const std::uint8_t& operator[](std::size_t i) const { return p[i]; }
};

uint32_t readU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t readU16(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                                 (static_cast<uint16_t>(p[1]) << 8));
}

std::string lowerExt(const std::string& filename) {
    const size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos) return std::string();
    std::string ext = filename.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

/**
 * Inflate `src` into `out`.
 *
 * @param raw  true => raw deflate (zip), false => zlib/gzip wrapper decided by
 *             `window_bits` (pass 16+MAX_WBITS for gzip).
 */
bool inflateInto(const uint8_t* src, std::size_t src_len, int window_bits,
                 std::vector<uint8_t>& out, std::size_t cap, std::string& error) {
    z_stream strm;
    std::memset(&strm, 0, sizeof(strm));
    if (inflateInit2(&strm, window_bits) != Z_OK) {
        error = "zlib: inflateInit2 failed";
        return false;
    }

    out.clear();
    const std::size_t chunk = 256 * 1024;
    int rc = Z_OK;
    while (rc != Z_STREAM_END) {
        // Grow by whole chunks, but never past the cap: a small entry must not
        // fail just because one chunk is bigger than the entry itself.
        const std::size_t want = std::min<std::size_t>(chunk, cap - out.size());
        if (want == 0) {
            inflateEnd(&strm);
            error = "decompressed entry exceeds the size limit";
            return false;
        }
        out.resize(out.size() + want);
        strm.next_in   = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(src));
        strm.avail_in  = static_cast<uInt>(src_len);
        strm.next_out  = reinterpret_cast<Bytef*>(out.data() + (out.size() - want));
        strm.avail_out = static_cast<uInt>(want);

        rc = inflate(&strm, Z_FINISH);
        if (rc != Z_STREAM_END && rc != Z_OK && rc != Z_BUF_ERROR) {
            inflateEnd(&strm);
            error = std::string("zlib: inflate failed (rc=") + std::to_string(rc) + ")";
            if (strm.msg) error += std::string(": ") + strm.msg;
            return false;
        }
        if (rc == Z_BUF_ERROR && strm.avail_in == 0) {
            inflateEnd(&strm);
            error = "zlib: truncated deflate stream";
            return false;
        }
        const std::size_t produced = want - strm.avail_out;
        out.resize(out.size() - want + produced);
        if (rc == Z_OK && produced == 0 && strm.avail_in == 0) break;  // no progress
    }
    inflateEnd(&strm);
    return true;
}

// ------------------------------------------------------------------ ZIP ----

bool readZip(const ByteView data, std::vector<ArchiveEntry>& out,
             std::string& error, const ArchiveLimits& limits) {
    // End Of Central Directory: signature 0x06054b50, located in the last
    // 22 + 65535 bytes (the comment may be up to 64 KiB).
    const std::size_t max_back = std::min<std::size_t>(data.size(), 22 + 65535);
    std::size_t eocd = std::string::npos;
    for (std::size_t i = data.size() - max_back; i + 4 <= data.size(); ++i) {
        if (data[i] == 0x50 && data[i + 1] == 0x4b && data[i + 2] == 0x05 &&
            data[i + 3] == 0x06) {
            eocd = i;  // keep scanning: a comment could contain the signature
        }
    }
    if (eocd == std::string::npos) {
        error = "not a zip archive (end-of-central-directory not found)";
        return false;
    }

    const uint16_t entry_count = readU16(&data[eocd + 10]);
    const uint32_t cd_size     = readU32(&data[eocd + 12]);
    const uint32_t cd_offset32 = readU32(&data[eocd + 16]);

    std::size_t cd_offset = cd_offset32;
    std::size_t count     = entry_count;
    const bool  zip64     = (cd_offset32 == 0xFFFFFFFFu || entry_count == 0xFFFFu ||
                             cd_size == 0xFFFFFFFFu);
    if (zip64) {
        // Zip64 EOCD locator sits just before the EOCD.
        if (eocd < 20) {
            error = "zip64 archive: locator missing";
            return false;
        }
        const std::size_t loc = eocd - 20;
        if (!(data[loc] == 0x50 && data[loc + 1] == 0x4b && data[loc + 2] == 0x06 &&
              data[loc + 3] == 0x07)) {
            error = "zip64 archive: unsupported layout";
            return false;
        }
        // Zip64 EOCD record sits before the locator and is 56 bytes long:
        // signature 0x06064b50, entry count at +32, cd offset at +48.
        if (loc < 56) {
            error = "zip64 archive: EOCD record missing";
            return false;
        }
        const std::size_t z64 = loc - 56;
        if (z64 + 56 > data.size() || readU32(&data[z64]) != 0x06064b50u) {
            error = "zip64 archive: EOCD record missing";
            return false;
        }
        uint64_t n = 0, off = 0;
        std::memcpy(&n,   &data[z64 + 32], sizeof(n));
        std::memcpy(&off, &data[z64 + 48], sizeof(off));
        count     = static_cast<std::size_t>(n);
        cd_offset = static_cast<std::size_t>(off);
    }

    if (cd_offset > data.size()) {
        error = "corrupt zip: central directory out of range";
        return false;
    }

    std::size_t p = cd_offset;
    std::size_t total = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (p + 46 > data.size() || readU32(&data[p]) != 0x02014b50u) break;

        const uint16_t method   = readU16(&data[p + 10]);
        const uint32_t comp32   = readU32(&data[p + 20]);
        const uint32_t uncomp32 = readU32(&data[p + 24]);
        const uint16_t name_len = readU16(&data[p + 28]);
        const uint16_t extra_len = readU16(&data[p + 30]);
        const uint16_t comment_len = readU16(&data[p + 32]);
        uint32_t local_off32 = readU32(&data[p + 42]);

        std::string name(reinterpret_cast<const char*>(&data[p + 46]), name_len);

        std::size_t local_off = local_off32;
        std::size_t comp_size = comp32;
        std::size_t uncomp    = uncomp32;
        if (local_off32 == 0xFFFFFFFFu || comp32 == 0xFFFFFFFFu ||
            uncomp32 == 0xFFFFFFFFu) {
            // Zip64 extra field (0x0001) inside the central directory record.
            const std::size_t ex = p + 46 + name_len;
            std::size_t q = ex;
            const std::size_t ex_end = std::min<std::size_t>(ex + extra_len, data.size());
            while (q + 4 <= ex_end) {
                const uint16_t tag = readU16(&data[q]);
                const uint16_t sz  = readU16(&data[q + 2]);
                const std::size_t body = q + 4;
                if (tag == 0x0001) {
                    std::size_t f = body;
                    if (uncomp32 == 0xFFFFFFFFu && f + 8 <= ex_end) {
                        uint64_t v; std::memcpy(&v, &data[f], sizeof(v));
                        uncomp = static_cast<std::size_t>(v); f += 8;
                    }
                    if (comp32 == 0xFFFFFFFFu && f + 8 <= ex_end) {
                        uint64_t v; std::memcpy(&v, &data[f], sizeof(v));
                        comp_size = static_cast<std::size_t>(v); f += 8;
                    }
                    if (local_off32 == 0xFFFFFFFFu && f + 8 <= ex_end) {
                        uint64_t v; std::memcpy(&v, &data[f], sizeof(v));
                        local_off = static_cast<std::size_t>(v);
                    }
                    break;
                }
                q = body + sz;
            }
        }

        p += 46 + name_len + extra_len + comment_len;

        if (name.empty() || name.back() == '/') continue;  // directory entry
        if (local_off + 30 > data.size()) continue;

        // The local header repeats the name/extra lengths and they are the
        // authoritative ones for locating the data.
        if (readU32(&data[local_off]) != 0x04034b50u) continue;
        const uint16_t l_name  = readU16(&data[local_off + 26]);
        const uint16_t l_extra = readU16(&data[local_off + 28]);
        const std::size_t data_off = local_off + 30 + l_name + l_extra;
        if (data_off + comp_size > data.size()) continue;

        ArchiveEntry entry;
        const size_t slash = name.find_last_of("/\\");
        entry.path     = name;
        entry.basename = (slash == std::string::npos) ? name : name.substr(slash + 1);

        if (uncomp > limits.max_entry_bytes) {
            error = "entry '" + entry.basename + "' is larger than the per-file limit";
            return false;
        }
        if (method == 0) {
            entry.data.assign(data.begin() + data_off,
                              data.begin() + data_off + comp_size);
        } else if (method == 8) {
            std::vector<uint8_t> tmp;
            if (!inflateInto(&data[data_off], comp_size, -MAX_WBITS, tmp,
                             std::min<std::size_t>(uncomp + 1, limits.max_entry_bytes + 1),
                             error)) {
                error = "entry '" + entry.basename + "': " + error;
                return false;
            }
            entry.data = std::move(tmp);
        } else {
            // Unsupported method (e.g. bzip2/ AES): skip rather than abort,
            // the remaining images are still useful.
            continue;
        }

        total += entry.data.size();
        if (total > limits.max_total_bytes) {
            error = "archive decompresses to more than the allowed total size";
            return false;
        }
        if (out.size() >= limits.max_entries) {
            error = "archive contains more entries than the allowed limit";
            return false;
        }
        out.push_back(std::move(entry));
    }
    return true;
}

// ------------------------------------------------------------------ TAR ----

/** Tar size field: octal string, or base-256 when the top bit is set. */
uint64_t tarSize(const uint8_t* h) {
    if (h[124] & 0x80) {
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v = (v << 8) | static_cast<uint64_t>(h[124 + i] & 0x7F);
        }
        return v;
    }
    uint64_t v = 0;
    for (int i = 0; i < 12; ++i) {
        const char c = static_cast<char>(h[124 + i]);
        if (c == '\0' || c == ' ') break;
        if (c < '0' || c > '7') continue;
        v = v * 8 + static_cast<uint64_t>(c - '0');
    }
    return v;
}

std::string tarString(const uint8_t* p, std::size_t n) {
    const uint8_t* end = p + n;
    while (end > p && (end[-1] == '\0' || end[-1] == ' ')) --end;
    return std::string(reinterpret_cast<const char*>(p), static_cast<std::size_t>(end - p));
}

bool readTar(const ByteView data, std::vector<ArchiveEntry>& out,
             std::string& error, const ArchiveLimits& limits) {
    const std::size_t kBlock = 512;
    std::size_t p = 0;
    std::size_t total = 0;
    std::string long_name;  // GNU 'L' entry supplies the next entry's name

    while (p + kBlock <= data.size()) {
        const uint8_t* h = &data[p];
        // Two zero blocks mark the end of the archive.
        bool empty = true;
        for (std::size_t i = 0; i < kBlock; ++i) {
            if (h[i] != 0) { empty = false; break; }
        }
        if (empty) break;

        std::string name;
        if (!long_name.empty()) {
            name = long_name;
            long_name.clear();
        } else {
            name = tarString(h, 100);
            // ustar splits long paths into prefix + name.
            const std::string prefix = tarString(h + 345, 155);
            if (!prefix.empty()) name = prefix + "/" + name;
        }
        const char     typeflag = static_cast<char>(h[156]);
        const uint64_t size     = tarSize(h);

        p += kBlock;
        const std::size_t payload = static_cast<std::size_t>(size);
        if (payload > data.size() - p) {
            error = "corrupt tar: entry extends past the end of the archive";
            return false;
        }

        if (typeflag == 'L') {  // GNU long name for the NEXT header
            long_name.assign(reinterpret_cast<const char*>(&data[p]), payload);
            while (!long_name.empty() && long_name.back() == '\0') long_name.pop_back();
        } else if (typeflag == '0' || typeflag == '\0' || typeflag == '7') {
            if (!name.empty() && name.back() != '/') {
                if (payload > limits.max_entry_bytes) {
                    error = "entry '" + name + "' is larger than the per-file limit";
                    return false;
                }
                ArchiveEntry entry;
                const size_t slash = name.find_last_of("/\\");
                entry.path     = name;
                entry.basename = (slash == std::string::npos) ? name : name.substr(slash + 1);
                entry.data.assign(data.begin() + p, data.begin() + p + payload);

                total += entry.data.size();
                if (total > limits.max_total_bytes) {
                    error = "archive decompresses to more than the allowed total size";
                    return false;
                }
                if (out.size() >= limits.max_entries) {
                    error = "archive contains more entries than the allowed limit";
                    return false;
                }
                out.push_back(std::move(entry));
            }
        }
        // 'x'/'g' (pax headers), links, devices: skipped on purpose.

        p += payload;
        p += (kBlock - (payload % kBlock)) % kBlock;  // payload is block-aligned
    }
    return true;
}

}  // namespace

ArchiveKind detectArchiveKind(const std::string& filename,
                              const std::uint8_t* data_ptr, std::size_t size) {
    const ByteView data{data_ptr, size};
    if (data.size() >= 2 && data[0] == 0x1f && data[1] == 0x8b) return ArchiveKind::Gzip;
    if (data.size() >= 4 && data[0] == 0x50 && data[1] == 0x4b &&
        (data[2] == 0x03 || data[2] == 0x05 || data[2] == 0x07)) {
        return ArchiveKind::Zip;
    }
    if (data.size() >= 262 && std::memcmp(&data[257], "ustar", 5) == 0) {
        return ArchiveKind::Tar;
    }
    const std::string ext = lowerExt(filename);
    if (ext == "zip") return ArchiveKind::Zip;
    if (ext == "gz" || ext == "tgz") return ArchiveKind::Gzip;
    if (ext == "tar") return ArchiveKind::Tar;
    return ArchiveKind::Unknown;
}

bool readArchive(const std::uint8_t* data_ptr, std::size_t size,
                 const std::string& filename,
                 std::vector<ArchiveEntry>& out, std::string& error,
                 const ArchiveLimits& limits) {
    out.clear();
    if (data_ptr == nullptr || size == 0) {
        error = "empty archive";
        return false;
    }
    const ByteView data{data_ptr, size};

    switch (detectArchiveKind(filename, data_ptr, size)) {
        case ArchiveKind::Zip:
            return readZip(data, out, error, limits);

        case ArchiveKind::Tar:
            return readTar(data, out, error, limits);

        case ArchiveKind::Gzip: {
            // gzip must be expanded before the tar inside can be walked, so
            // this is the one step that genuinely needs its own buffer.
            std::vector<uint8_t> tar;
            if (!inflateInto(data.data(), data.size(), 16 + MAX_WBITS, tar,
                             limits.max_total_bytes, error)) {
                return false;
            }
            if (tar.empty()) {
                error = "gzip stream contains no data (only .tar.gz / .tgz are supported)";
                return false;
            }
            return readTar(ByteView{tar.data(), tar.size()}, out, error, limits);
        }

        case ArchiveKind::Unknown:
        default:
            error = "unsupported archive format (expected .zip / .tar / .tar.gz / .tgz)";
            return false;
    }
}

}  // namespace face_db_web
