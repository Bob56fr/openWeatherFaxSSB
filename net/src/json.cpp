// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 openWeatherFaxSSB contributors

#include "owfax/net/json.hpp"

#include <cstdio>
#include <cstdlib>

namespace owfax::net {
namespace {

constexpr int kMaxDepth = 32;

class Parser {
public:
    explicit Parser(std::string_view t) : t_(t) {}

    std::optional<Json> document() {
        Json v;
        if (!value(v, 0)) return std::nullopt;
        skip_ws();
        if (p_ != t_.size()) return std::nullopt;
        return v;
    }

private:
    void skip_ws() {
        while (p_ < t_.size() &&
               (t_[p_] == ' ' || t_[p_] == '\t' || t_[p_] == '\n' || t_[p_] == '\r')) {
            ++p_;
        }
    }

    bool literal(const char* word) {
        std::size_t n = 0;
        while (word[n] != '\0') ++n;
        if (t_.substr(p_, n) != std::string_view(word, n)) return false;
        p_ += n;
        return true;
    }

    static void put_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned& v) {
        if (p_ + 4 > t_.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = t_[p_++];
            v <<= 4;
            if (c >= '0' && c <= '9') {
                v |= static_cast<unsigned>(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                v |= static_cast<unsigned>(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                v |= static_cast<unsigned>(c - 'A' + 10);
            } else {
                return false;
            }
        }
        return true;
    }

    bool str(std::string& out) {
        if (p_ >= t_.size() || t_[p_] != '"') return false;
        ++p_;
        while (p_ < t_.size()) {
            const char c = t_[p_++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (p_ >= t_.size()) return false;
            const char e = t_[p_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp < 0xDC00) {  // surrogate pair
                        unsigned lo = 0;
                        if (!literal("\\u") || !hex4(lo) || lo < 0xDC00 || lo >= 0xE000)
                            return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (cp >= 0xDC00 && cp < 0xE000) {
                        return false;
                    }
                    put_utf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    bool num(double& out) {
        const std::size_t start = p_;
        if (p_ < t_.size() && t_[p_] == '-') ++p_;
        if (p_ >= t_.size() || t_[p_] < '0' || t_[p_] > '9') return false;
        while (p_ < t_.size() &&
               ((t_[p_] >= '0' && t_[p_] <= '9') || t_[p_] == '.' || t_[p_] == 'e' ||
                t_[p_] == 'E' || t_[p_] == '+' || t_[p_] == '-')) {
            ++p_;
        }
        const std::string s(t_.substr(start, p_ - start));
        char* end = nullptr;
        out = std::strtod(s.c_str(), &end);
        return end != nullptr && *end == '\0';
    }

    bool value(Json& v, int depth) {
        if (depth > kMaxDepth) return false;
        skip_ws();
        if (p_ >= t_.size()) return false;
        const char c = t_[p_];
        if (c == '{') {
            ++p_;
            v.type = Json::Type::kObject;
            skip_ws();
            if (p_ < t_.size() && t_[p_] == '}') {
                ++p_;
                return true;
            }
            while (true) {
                skip_ws();
                std::string key;
                if (!str(key)) return false;
                skip_ws();
                if (p_ >= t_.size() || t_[p_] != ':') return false;
                ++p_;
                Json member;
                if (!value(member, depth + 1)) return false;
                v.object[key] = std::move(member);
                skip_ws();
                if (p_ < t_.size() && t_[p_] == ',') {
                    ++p_;
                    continue;
                }
                if (p_ < t_.size() && t_[p_] == '}') {
                    ++p_;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            ++p_;
            v.type = Json::Type::kArray;
            skip_ws();
            if (p_ < t_.size() && t_[p_] == ']') {
                ++p_;
                return true;
            }
            while (true) {
                Json item;
                if (!value(item, depth + 1)) return false;
                v.array.push_back(std::move(item));
                skip_ws();
                if (p_ < t_.size() && t_[p_] == ',') {
                    ++p_;
                    continue;
                }
                if (p_ < t_.size() && t_[p_] == ']') {
                    ++p_;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            v.type = Json::Type::kString;
            return str(v.string);
        }
        if (literal("true")) {
            v.type = Json::Type::kBool;
            v.boolean = true;
            return true;
        }
        if (literal("false")) {
            v.type = Json::Type::kBool;
            return true;
        }
        if (literal("null")) return true;
        v.type = Json::Type::kNumber;
        return num(v.number);
    }

    std::string_view t_;
    std::size_t p_ = 0;
};

}  // namespace

const Json* Json::get(const std::string& key) const {
    if (type != Type::kObject) return nullptr;
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &it->second;
}

std::optional<Json> parse_json(std::string_view text) {
    return Parser(text).document();
}

std::string json_quote(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

}  // namespace owfax::net
