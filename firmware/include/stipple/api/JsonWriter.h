// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace stipple {
namespace api {

/// Minimal streaming JSON writer.
///
/// Tracks nesting so commas land in the right places and strings are escaped
/// once, in one function. Hand-assembling response bodies with string
/// concatenation is where malformed JSON and injection bugs come from, and the
/// API is public surface that integrations will parse strictly.
///
/// Writing only — reading is the parser's job (ADR 0014).
class JsonWriter {
public:
    JsonWriter() { stack_.reserve(8); }

    JsonWriter& beginObject();
    JsonWriter& endObject();
    JsonWriter& beginArray();
    JsonWriter& endArray();

    /// Key for the next value inside an object.
    JsonWriter& key(std::string_view name);

    JsonWriter& value(std::string_view text);
    JsonWriter& value(const char* text) { return value(std::string_view(text)); }
    JsonWriter& value(const std::string& text) { return value(std::string_view(text)); }
    JsonWriter& value(std::int64_t number);
    JsonWriter& value(int number) { return value(static_cast<std::int64_t>(number)); }
    JsonWriter& value(bool flag);
    JsonWriter& nullValue();

    /// Insert already-valid JSON verbatim, for embedding a stored scene document
    /// without reparsing and re-serialising it.
    JsonWriter& rawValue(std::string_view json);

    // Convenience: key and value together.
    JsonWriter& member(std::string_view name, std::string_view text);
    JsonWriter& member(std::string_view name, const char* text) {
        return member(name, std::string_view(text));
    }
    JsonWriter& member(std::string_view name, const std::string& text) {
        return member(name, std::string_view(text));
    }
    JsonWriter& member(std::string_view name, std::int64_t number);
    JsonWriter& member(std::string_view name, int number) {
        return member(name, static_cast<std::int64_t>(number));
    }
    JsonWriter& member(std::string_view name, bool flag);
    JsonWriter& rawMember(std::string_view name, std::string_view json);
    /// Splice already-valid `"key":value,...` members into the open object,
    /// for a block whose shape is owned by another module (the stored file
    /// and the API must write it identically).
    JsonWriter& rawMembers(std::string_view members);

    std::string take() { return std::move(out_); }
    const std::string& str() const { return out_; }

private:
    void separate();

    std::string out_;
    /// One entry per open container; true once it holds something, so the next
    /// write knows whether to prefix a comma.
    std::vector<bool> stack_;
    bool expectingValue_ = false;
};

/// Escape `text` as a JSON string, including the surrounding quotes.
void appendJsonString(std::string& out, std::string_view text);

}  // namespace api
}  // namespace stipple
