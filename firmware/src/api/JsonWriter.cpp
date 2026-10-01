// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/api/JsonWriter.h"

namespace stipple {
namespace api {

void appendJsonString(std::string& out, std::string_view text) {
    static const char kHex[] = "0123456789abcdef";

    out.push_back('"');
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20u) {
                    out += "\\u00";
                    out.push_back(kHex[(static_cast<unsigned char>(c) >> 4) & 0xFu]);
                    out.push_back(kHex[static_cast<unsigned char>(c) & 0xFu]);
                } else {
                    // Bytes >= 0x80 pass through: the input is already UTF-8 and
                    // re-encoding it as \u escapes would only make it larger.
                    out.push_back(c);
                }
                break;
        }
    }
    out.push_back('"');
}

void JsonWriter::separate() {
    if (expectingValue_) {
        expectingValue_ = false;
        return;  // a key was just written; the colon is already there
    }
    if (stack_.empty()) {
        return;
    }
    if (stack_.back()) {
        out_.push_back(',');
    } else {
        stack_.back() = true;
    }
}

JsonWriter& JsonWriter::beginObject() {
    separate();
    out_.push_back('{');
    stack_.push_back(false);
    return *this;
}

JsonWriter& JsonWriter::endObject() {
    if (!stack_.empty()) {
        stack_.pop_back();
    }
    out_.push_back('}');
    return *this;
}

JsonWriter& JsonWriter::beginArray() {
    separate();
    out_.push_back('[');
    stack_.push_back(false);
    return *this;
}

JsonWriter& JsonWriter::endArray() {
    if (!stack_.empty()) {
        stack_.pop_back();
    }
    out_.push_back(']');
    return *this;
}

JsonWriter& JsonWriter::key(std::string_view name) {
    separate();
    appendJsonString(out_, name);
    out_.push_back(':');
    expectingValue_ = true;
    return *this;
}

JsonWriter& JsonWriter::value(std::string_view text) {
    separate();
    appendJsonString(out_, text);
    return *this;
}

JsonWriter& JsonWriter::value(std::int64_t number) {
    separate();
    out_ += std::to_string(number);
    return *this;
}

JsonWriter& JsonWriter::value(bool flag) {
    separate();
    out_ += flag ? "true" : "false";
    return *this;
}

JsonWriter& JsonWriter::nullValue() {
    separate();
    out_ += "null";
    return *this;
}

JsonWriter& JsonWriter::rawValue(std::string_view json) {
    separate();
    if (json.empty()) {
        out_ += "null";
    } else {
        out_ += json;
    }
    return *this;
}

JsonWriter& JsonWriter::member(std::string_view name, std::string_view text) {
    return key(name).value(text);
}

JsonWriter& JsonWriter::member(std::string_view name, std::int64_t number) {
    return key(name).value(number);
}

JsonWriter& JsonWriter::member(std::string_view name, bool flag) {
    return key(name).value(flag);
}

JsonWriter& JsonWriter::rawMember(std::string_view name, std::string_view json) {
    return key(name).rawValue(json);
}

JsonWriter& JsonWriter::rawMembers(std::string_view members) {
    if (members.empty()) {
        return *this;
    }
    separate();
    out_.append(members);
    return *this;
}

}  // namespace api
}  // namespace stipple
