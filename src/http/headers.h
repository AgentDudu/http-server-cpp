#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace http {

// Field name / value validity per RFC 7230 token + field-content rules.
bool validFieldName(std::string_view name);
bool validFieldValue(std::string_view value);

// Ordered header list with case-insensitive lookup. Values are trimmed of
// leading/trailing SP and HTAB on insertion (RFC 7230 obs-fold removal semantics).
class Headers {
  public:
    struct Entry {
        std::string name;
        std::string value;
    };

    void set(std::string name, std::string value); // replaces every existing match
    void add(std::string name, std::string value);
    const std::string* get(std::string_view name) const;
    bool contains(std::string_view name) const { return get(name) != nullptr; }
    const std::vector<Entry>& entries() const { return entries_; }

  private:
    std::size_t find(std::string_view name) const;

    std::vector<Entry> entries_;
};

} // namespace http
