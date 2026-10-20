#pragma once

#include "server/handler.h"

#include <filesystem>
#include <string_view>

namespace server {

// Maps request targets to files under a canonical root. GET/HEAD only (405);
// traversal, encoded traversal, absolute targets and backslashes are rejected
// as 404 before touching the disk. Directories serve index.html; unknown
// extensions get application/octet-stream. Whole-file reads (streaming and
// conditional requests are the caching phase).
class StaticFiles {
public:
    // Throws std::filesystem::filesystem_error when root does not exist.
    explicit StaticFiles(std::filesystem::path root);

    Reply operator()(const http::Request& req) const;

    // Decoded, normalised, containment-checked absolute path; false = refuse.
    // Public so tests hammer the resolver directly.
    bool resolve(std::string_view target, std::filesystem::path& out) const;

    const std::filesystem::path& root() const { return root_; }

private:
    bool contained(const std::filesystem::path& candidate) const;

    std::filesystem::path root_; // canonical, exists
};

// Extension → media type (case-insensitive); octet-stream fallback.
std::string_view mimeFor(std::string_view filename);

} // namespace server
