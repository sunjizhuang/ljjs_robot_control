#include "json.h"
#include <cctype>
#include <cstdlib>

/* ── JSON 边界检测 ── */
size_t json_find_end(const std::string& buf, size_t start)
{
    int depth = 0;
    bool in_string = false;
    bool escape = false;

    for (size_t i = start; i < buf.size(); i++) {
        char c = buf[i];
        if (escape) { escape = false; continue; }
        if (in_string) {
            if (c == '\\') { escape = true; continue; }
            if (c == '"')  { in_string = false; }
            continue;
        }
        if (c == '"') { in_string = true; continue; }
        if (c == '{' || c == '[') depth++;
        if (c == '}' || c == ']') {
            depth--;
            if (depth == 0) return i + 1;
        }
    }
    return std::string::npos;
}

/* ── 递归下降解析器 ── */
class Parser {
public:
    Parser(const char* s, const char* e) : cur_(s), end_(e) {}

    bool parse(JsonValue& out) {
        skipWs();
        return parseValue(out);
    }

private:
    const char* cur_;
    const char* end_;

    void skipWs() {
        while (cur_ < end_ && std::isspace((unsigned char)*cur_)) cur_++;
    }

    bool parseValue(JsonValue& v) {
        skipWs();
        if (cur_ >= end_) return false;
        char c = *cur_;
        if (c == '{') return parseObject(v);
        if (c == '[') return parseArray(v);
        if (c == '"') return parseString(v);
        if (c == 't' || c == 'f') return parseBool(v);
        if (c == 'n') return parseNull(v);
        return parseNumber(v);
    }

    bool parseObject(JsonValue& v) {
        v.type_ = JsonValue::OBJECT;
        cur_++; // skip {
        skipWs();
        if (cur_ < end_ && *cur_ == '}') { cur_++; return true; }

        while (cur_ < end_) {
            skipWs();
            if (*cur_ != '"') return false;

            JsonValue key;
            if (!parseString(key)) return false;
            skipWs();
            if (cur_ >= end_ || *cur_ != ':') return false;
            cur_++; // skip :

            JsonValue val;
            if (!parseValue(val)) return false;
            v.obj_[key.str_] = val;

            skipWs();
            if (cur_ >= end_) return false;
            if (*cur_ == ',') { cur_++; continue; }
            if (*cur_ == '}') { cur_++; return true; }
            return false;
        }
        return false;
    }

    bool parseArray(JsonValue& v) {
        v.type_ = JsonValue::ARRAY;
        cur_++; // skip [
        skipWs();
        if (cur_ < end_ && *cur_ == ']') { cur_++; return true; }

        while (cur_ < end_) {
            JsonValue val;
            if (!parseValue(val)) return false;
            v.arr_.push_back(val);
            skipWs();
            if (cur_ >= end_) return false;
            if (*cur_ == ',') { cur_++; continue; }
            if (*cur_ == ']') { cur_++; return true; }
            return false;
        }
        return false;
    }

    bool parseString(JsonValue& v) {
        v.type_ = JsonValue::STRING;
        cur_++; // skip "
        while (cur_ < end_) {
            char c = *cur_++;
            if (c == '"') return true;
            if (c == '\\' && cur_ < end_) {
                char esc = *cur_++;
                switch (esc) {
                    case '"':  v.str_ += '"';  break;
                    case '\\': v.str_ += '\\'; break;
                    case '/':  v.str_ += '/';  break;
                    case 'n':  v.str_ += '\n'; break;
                    case 't':  v.str_ += '\t'; break;
                    case 'r':  v.str_ += '\r'; break;
                    case 'b':  v.str_ += '\b'; break;
                    case 'f':  v.str_ += '\f'; break;
                    case 'u':
                        if (cur_ + 4 <= end_) {
                            /* 简单跳过 \uXXXX */
                            cur_ += 4;
                            v.str_ += '?';
                        }
                        break;
                    default: v.str_ += esc; break;
                }
            } else {
                v.str_ += c;
            }
        }
        return false;
    }

    bool parseNumber(JsonValue& v) {
        v.type_ = JsonValue::NUMBER;
        const char* start = cur_;
        if (cur_ < end_ && (*cur_ == '-' || *cur_ == '+')) cur_++;
        while (cur_ < end_ && (std::isdigit((unsigned char)*cur_) || *cur_ == '.' || *cur_ == 'e' || *cur_ == 'E' || *cur_ == '+' || *cur_ == '-'))
            cur_++;
        std::string num_str(start, cur_);
        v.num_ = strtod(num_str.c_str(), nullptr);
        return true;
    }

    bool parseBool(JsonValue& v) {
        v.type_ = JsonValue::BOOL;
        if (cur_ + 4 <= end_ && cur_[0] == 't' && cur_[1] == 'r' && cur_[2] == 'u' && cur_[3] == 'e') {
            v.bool_ = true;
            cur_ += 4;
            return true;
        }
        if (cur_ + 5 <= end_ && cur_[0] == 'f' && cur_[1] == 'a' && cur_[2] == 'l' && cur_[3] == 's' && cur_[4] == 'e') {
            v.bool_ = false;
            cur_ += 5;
            return true;
        }
        return false;
    }

    bool parseNull(JsonValue& v) {
        v.type_ = JsonValue::NUL;
        if (cur_ + 4 <= end_ && cur_[0] == 'n' && cur_[1] == 'u' && cur_[2] == 'l' && cur_[3] == 'l') {
            cur_ += 4;
            return true;
        }
        return false;
    }
};

bool json_parse(const std::string& text, JsonValue& out)
{
    Parser p(text.c_str(), text.c_str() + text.size());
    return p.parse(out);
}
