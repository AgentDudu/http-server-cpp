#include "http/headers.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace http {
namespace {

char lowerAscii(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool nameEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lowerAscii(a[i]) != lowerAscii(b[i])) {
            return false;
        }
    }
    return true;
}

bool isTokenChar(char c) {
    // tchar = "!" / "#" / "$" / "%" / "&" / "'" / "*" / "+" / "-" / "." / "^" /
    //         "_" / "`" / "|" / "~" / DIGIT / ALPHA
    static const char* extras = "!#$%&'*+-.^_`|~";
    return std::isalnum(static_cast<unsigned char>(c)) != 0 ||
           std::strchr(extras, c) != nullptr;
}

std::string trimField(std::string value) {
    const char* ws = " \t";
    const auto begin = value.find_first_not_of(ws);
    if (begin == std::string::npos) {
        value.clear();
        return value;
    }
    const auto end = value.find_last_not_of(ws);
    value.erase(end + 1);
    value.erase(0, begin);
    return value;
}

} // namespace

bool validFieldName(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), isTokenChar);
}

bool validFieldValue(std::string_view value) {
    for (const char c : value) {
        const unsigned char u = static_cast<unsigned char>(c);
        // field-content = VCHAR / obs-text; SP and HTAB allowed between them.
        // Reject CTLs (incl. NUL, CR, LF), DEL.
        if (u < 0x20 || u == 0x7F) {
            if (u != '\t') {
                return false;
            }
        }
    }
    return true;
}

void Headers::set(std::string name, std::string value) {
    const std::string trimmed = trimField(std::move(value));
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const Entry& e) { return nameEquals(e.name, name); }),
                   entries_.end());
    entries_.push_back({std::move(name), trimmed});
}

void Headers::add(std::string name, std::string value) {
    entries_.push_back({std::move(name), trimField(std::move(value))});
}

std::size_t Headers::find(std::string_view name) const {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
        if (nameEquals(entries_[i].name, name)) {
            return i;
        }
    }
    return entries_.size();
}

const std::string* Headers::get(std::string_view name) const {
    const std::size_t i = find(name);
    return i == entries_.size() ? nullptr : &entries_[i].value;
}

} // namespace http
