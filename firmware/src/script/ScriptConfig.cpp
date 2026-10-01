// SPDX-License-Identifier: GPL-3.0-or-later
#include "stipple/script/ScriptConfig.h"

namespace stipple {
namespace script {
namespace {

bool space(char c) noexcept { return c == ' ' || c == '\t'; }

bool lower(char c) noexcept { return c >= 'a' && c <= 'z'; }
bool digit(char c) noexcept { return c >= '0' && c <= '9'; }

/// Keys become store keys and form-field names, so the set is deliberately
/// narrow: lowercase, digits, dash, underscore. Wider would mean deciding
/// what a key containing a quote does to the page that renders it.
bool validKey(std::string_view key) noexcept {
    if (key.empty() || key.size() > kMaxSettingKeyBytes) {
        return false;
    }
    if (!lower(key.front()) && key.front() != '_') {
        return false;
    }
    for (const char c : key) {
        if (!lower(c) && !digit(c) && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

/// One token: a bare word, or a quoted run, or `name="quoted run"`.
///
/// Written out rather than split on spaces, because the label and the help
/// text both contain spaces and both matter. A split would have made
/// `"Channel ID"` two tokens and `help="not the @handle"` four.
struct Lexer {
    std::string_view text;
    std::size_t at = 0;

    bool next(std::string& out) {
        while (at < text.size() && space(text[at])) {
            ++at;
        }
        if (at >= text.size()) {
            return false;
        }

        out.clear();
        bool quoted = false;

        while (at < text.size()) {
            const char c = text[at];
            if (c == '"') {
                quoted = !quoted;
                ++at;
                continue;
            }
            if (!quoted && space(c)) {
                break;
            }
            out += c;
            ++at;
        }
        // An unterminated quote consumes the rest of the line rather than
        // failing. The alternative is discarding a declaration over a missing
        // character at the end, when what the author meant is obvious.
        return true;
    }
};

bool parseNumber(std::string_view text, long long& out) noexcept {
    if (text.empty() || text.size() > 19) {
        return false;
    }
    bool negative = false;
    std::size_t at = 0;
    if (text[0] == '-') {
        negative = true;
        at = 1;
        if (text.size() == 1) {
            return false;
        }
    }
    long long value = 0;
    for (; at < text.size(); ++at) {
        if (!digit(text[at])) {
            return false;
        }
        value = value * 10 + (text[at] - '0');
    }
    out = negative ? -value : value;
    return true;
}

void clamp(std::string& text, std::size_t limit) {
    if (text.size() > limit) {
        text.resize(limit);
    }
}

}  // namespace

const char* settingTypeName(Setting::Type type) noexcept {
    switch (type) {
        case Setting::Type::Text:    return "text";
        case Setting::Type::Number:  return "number";
        case Setting::Type::Boolean: return "boolean";
    }
    return "text";
}

std::vector<Setting> parseSettings(std::string_view source) {
    std::vector<Setting> settings;

    std::size_t at = 0;
    while (at < source.size() && settings.size() < kMaxSettings) {
        std::size_t end = source.find('\n', at);
        if (end == std::string_view::npos) {
            end = source.size();
        }
        std::string_view line = source.substr(at, end - at);
        at = end + 1;

        while (!line.empty() && (space(line.front()))) {
            line.remove_prefix(1);
        }
        while (!line.empty() && (space(line.back()) || line.back() == '\r')) {
            line.remove_suffix(1);
        }

        if (line.empty()) {
            continue;
        }
        if (line.front() != '#') {
            // The header is over. A @config further down is a comment about
            // the code beside it.
            break;
        }

        line.remove_prefix(1);
        while (!line.empty() && space(line.front())) {
            line.remove_prefix(1);
        }

        constexpr std::string_view kDirective = "@config";
        if (line.size() <= kDirective.size() ||
            line.substr(0, kDirective.size()) != kDirective ||
            !space(line[kDirective.size()])) {
            continue;
        }

        Lexer lexer{line.substr(kDirective.size()), 0};

        Setting setting;
        std::string token;

        if (!lexer.next(token) || !validKey(token)) {
            continue;
        }
        setting.key = token;

        if (!lexer.next(token)) {
            continue;
        }
        if (token == "text") {
            setting.type = Setting::Type::Text;
        } else if (token == "number") {
            setting.type = Setting::Type::Number;
        } else if (token == "boolean") {
            setting.type = Setting::Type::Boolean;
        } else {
            continue;  // an unknown type is a typo, not a new feature
        }

        if (!lexer.next(token) || token.empty()) {
            continue;  // an unlabelled field is one nobody can fill in
        }
        setting.label = token;
        clamp(setting.label, kMaxSettingTextBytes);

        while (lexer.next(token)) {
            const std::size_t equals = token.find('=');
            if (equals == std::string::npos || equals == 0) {
                continue;
            }
            const std::string name = token.substr(0, equals);
            std::string value = token.substr(equals + 1);

            if (name == "default") {
                setting.fallback = value;
                clamp(setting.fallback, kMaxSettingTextBytes);
            } else if (name == "help") {
                setting.help = value;
                clamp(setting.help, kMaxSettingTextBytes);
            } else if (name == "maxlen") {
                long long parsed = 0;
                if (parseNumber(value, parsed) && parsed > 0) {
                    setting.maxLength =
                        static_cast<int>(parsed > 4096 ? 4096 : parsed);
                }
            } else if (name == "min") {
                long long parsed = 0;
                if (parseNumber(value, parsed)) {
                    setting.minimum = parsed;
                    setting.bounded = true;
                }
            } else if (name == "max") {
                long long parsed = 0;
                if (parseNumber(value, parsed)) {
                    setting.maximum = parsed;
                    setting.bounded = true;
                }
            }
            // Anything else is ignored rather than refused: a newer script
            // using an option this firmware has not heard of should still
            // get its field.
        }

        // A key declared twice is a mistake, and the first one wins so the
        // result does not depend on how far down the file the reader got.
        bool duplicate = false;
        for (const Setting& held : settings) {
            if (held.key == setting.key) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            settings.push_back(std::move(setting));
        }
    }

    return settings;
}

InputMode parseInputMode(std::string_view source) {
    std::size_t at = 0;
    while (at < source.size()) {
        std::size_t end = source.find('\n', at);
        if (end == std::string_view::npos) {
            end = source.size();
        }
        std::string_view line = source.substr(at, end - at);
        at = end + 1;

        while (!line.empty() && space(line.front())) {
            line.remove_prefix(1);
        }
        while (!line.empty() && (space(line.back()) || line.back() == '\r')) {
            line.remove_suffix(1);
        }

        if (line.empty()) {
            continue;
        }
        if (line.front() != '#') {
            break;  // the header is over, same rule as parseSettings
        }

        line.remove_prefix(1);
        while (!line.empty() && space(line.front())) {
            line.remove_prefix(1);
        }

        constexpr std::string_view kDirective = "@input";
        if (line.size() <= kDirective.size() ||
            line.substr(0, kDirective.size()) != kDirective ||
            !space(line[kDirective.size()])) {
            continue;
        }

        Lexer lexer{line.substr(kDirective.size()), 0};
        std::string token;
        if (!lexer.next(token)) {
            continue;
        }
        if (token == "exclusive") {
            return InputMode::Exclusive;
        }
        // Anything else is a mode this firmware has not heard of. The script
        // keeps the default and still runs, rather than being refused by a
        // device that is merely older than it.
    }

    return InputMode::ActionOnly;
}

}  // namespace script
}  // namespace stipple
