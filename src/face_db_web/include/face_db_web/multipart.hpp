#pragma once

#ifndef FACE_DB_WEB_MULTIPART_HPP
#define FACE_DB_WEB_MULTIPART_HPP

/**
 * Minimal multipart/form-data reader.
 *
 * The admin UI sends bulk imports as multipart because a whole archive (or a
 * grid of images) cannot be squeezed into the x-www-form-urlencoded parser
 * without base64-inflating it first. Only the subset of RFC 7578 that a
 * browser actually emits is handled: a preamble, CRLF-separated part headers,
 * `Content-Disposition: form-data` with name/filename, and a closing
 * `--boundary--`.
 */

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace face_db_web {

struct MultipartPart {
    std::string name;          // form field name
    std::string filename;      // empty for plain (non-file) fields
    std::string content_type;  // part Content-Type, empty when absent
    std::string data;          // raw bytes; may contain NUL for file parts
};

class MultipartForm {
public:
    MultipartForm() = default;

    bool parse(const std::string& content_type, const std::string& body) {
        parts_.clear();
        error_.clear();

        std::string boundary = extractBoundary(content_type);
        if (boundary.empty()) {
            error_ = "Content-Type is not multipart or has no boundary";
            return false;
        }

        const std::string delim = "--" + boundary;
        size_t pos = body.find(delim);
        if (pos == std::string::npos) {
            error_ = "opening boundary not found in body";
            return false;
        }
        pos += delim.size();

        while (true) {
            // "--" right after the delimiter marks the closing boundary.
            if (body.compare(pos, 2, "--") == 0) break;

            if (body.compare(pos, 2, "\r\n") == 0) {
                pos += 2;
            } else if (pos < body.size() && body[pos] == '\n') {
                pos += 1;
            } else {
                error_ = "malformed part: expected CRLF after boundary";
                return false;
            }

            const size_t next = body.find(delim, pos);
            if (next == std::string::npos) {
                error_ = "unterminated part (closing boundary missing)";
                return false;
            }

            // The CRLF that precedes the next delimiter belongs to the
            // boundary, not to the part content.
            size_t content_end = next;
            if (content_end >= 2 && body.compare(content_end - 2, 2, "\r\n") == 0) {
                content_end -= 2;
            } else if (content_end >= 1 && body[content_end - 1] == '\n') {
                content_end -= 1;
            }

            MultipartPart part;
            const size_t body_start = parsePartHeaders(body, pos, content_end, part);
            if (body_start > content_end) {
                error_ = "malformed part headers";
                return false;
            }
            part.data = body.substr(body_start, content_end - body_start);
            parts_.push_back(std::move(part));

            pos = next + delim.size();
        }
        return true;
    }

    const std::string& error() const { return error_; }
    const std::vector<MultipartPart>& parts() const { return parts_; }

    /** First part with this field name, or nullptr. */
    const MultipartPart* first(const std::string& name) const {
        for (const auto& p : parts_) {
            if (p.name == name) return &p;
        }
        return nullptr;
    }

    /** Convenience: value of a plain (non-file) field, "" when absent. */
    std::string value(const std::string& name) const {
        if (const MultipartPart* p = first(name)) return p->data;
        return std::string();
    }

private:
    static std::string trim(const std::string& s) {
        size_t b = 0, e = s.size();
        while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
        return s.substr(b, e - b);
    }

    static std::string lower(const std::string& s) {
        std::string out = s;
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    /** `multipart/form-data; boundary=----X` (optionally quoted) -> `----X`. */
    static std::string extractBoundary(const std::string& content_type) {
        const std::string hay = lower(content_type);
        const size_t p = hay.find("boundary=");
        if (p == std::string::npos) return std::string();

        std::string b = content_type.substr(p + 9);
        const size_t semi = b.find(';');
        if (semi != std::string::npos) b = b.substr(0, semi);
        b = trim(b);
        if (b.size() >= 2 && b.front() == '"' && b.back() == '"') {
            b = b.substr(1, b.size() - 2);
        }
        return b;
    }

    /** Extracts `key="value"` from a Content-Disposition header. */
    static std::string dispositionParam(const std::string& header, const std::string& key) {
        const size_t p = header.find(key + "=");
        if (p == std::string::npos) return std::string();
        size_t i = p + key.size() + 1;
        while (i < header.size() && std::isspace(static_cast<unsigned char>(header[i]))) ++i;
        if (i < header.size() && header[i] == '"') {
            ++i;
            std::string out;
            while (i < header.size() && header[i] != '"') {
                if (header[i] == '\\' && i + 1 < header.size()) ++i;
                out.push_back(header[i]);
                ++i;
            }
            return out;
        }
        size_t end = i;
        while (end < header.size() && header[end] != ';' &&
               !std::isspace(static_cast<unsigned char>(header[end]))) {
            ++end;
        }
        return header.substr(i, end - i);
    }

    /** @return offset at which the part content starts. */
    static size_t parsePartHeaders(const std::string& body, size_t begin, size_t end,
                                   MultipartPart& part) {
        size_t sep = body.find("\r\n\r\n", begin);
        size_t skip = 4;
        if (sep == std::string::npos || sep + 4 > end) {
            sep = body.find("\n\n", begin);
            skip = 2;
        }
        if (sep == std::string::npos || sep + skip > end) {
            // No header block at all: an empty part.
            return begin;
        }

        const std::string block = body.substr(begin, sep - begin);
        size_t p = 0;
        while (p < block.size()) {
            size_t nl = block.find('\n', p);
            if (nl == std::string::npos) nl = block.size();
            std::string line = block.substr(p, nl - p);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t colon = line.find(':');
            if (colon != std::string::npos) {
                const std::string key = lower(trim(line.substr(0, colon)));
                const std::string val = trim(line.substr(colon + 1));
                if (key == "content-disposition") {
                    part.name     = dispositionParam(val, "name");
                    part.filename = dispositionParam(val, "filename");
                    // Some browsers still send a full client-side path.
                    const size_t slash = part.filename.find_last_of("/\\");
                    if (slash != std::string::npos) {
                        part.filename = part.filename.substr(slash + 1);
                    }
                } else if (key == "content-type") {
                    part.content_type = val;
                }
            }
            p = nl + 1;
        }
        return sep + skip;
    }

    std::vector<MultipartPart> parts_;
    std::string error_;
};

}  // namespace face_db_web

#endif  // FACE_DB_WEB_MULTIPART_HPP
