#ifndef JSON_H
#define JSON_H

#include <string>
#include <vector>
#include <map>
#include <cstdint>

/*
 * 极简 JSON 解析器 —— 仅需解析云迹机器人返回的 JSON 响应。
 * 支持类型: null, bool, number, string, array, object
 */
class JsonValue {
public:
    enum Type { NUL, BOOL, NUMBER, STRING, ARRAY, OBJECT };

    JsonValue() : type_(NUL), bool_(false), num_(0) {}

    Type type() const { return type_; }
    bool isNull()  const { return type_ == NUL; }
    bool isObject() const { return type_ == OBJECT; }
    bool isArray()  const { return type_ == ARRAY; }
    bool isString() const { return type_ == STRING; }

    /* 对象访问 */
    bool has(const std::string& key) const {
        return type_ == OBJECT && obj_.count(key) > 0;
    }
    const JsonValue& operator[](const std::string& key) const {
        static JsonValue null_val;
        auto it = obj_.find(key);
        return (it != obj_.end()) ? it->second : null_val;
    }

    /* 数组访问 */
    const JsonValue& operator[](size_t i) const {
        static JsonValue null_val;
        return (i < arr_.size()) ? arr_[i] : null_val;
    }
    size_t size() const { return type_ == ARRAY ? arr_.size() : 0; }

    /* 值提取 */
    bool        asBool()   const { return type_ == BOOL ? bool_ : false; }
    double      asNumber() const { return type_ == NUMBER ? num_ : 0; }
    int         asInt()    const { return type_ == NUMBER ? (int)num_ : 0; }
    std::string asString() const { return type_ == STRING ? str_ : ""; }

    /* 对象遍历 */
    typedef std::map<std::string, JsonValue>::const_iterator const_iterator;
    const_iterator begin() const { return obj_.begin(); }
    const_iterator end()   const { return obj_.end(); }

    // ── 内部字段（供解析器使用）──
    Type                                   type_  = NUL;
    bool                                   bool_  = false;
    double                                 num_   = 0;
    std::string                            str_;
    std::vector<JsonValue>                 arr_;
    std::map<std::string, JsonValue>       obj_;
};

/*
 * 解析 JSON 文本。成功返回 true，结果写入 out。
 */
bool json_parse(const std::string& text, JsonValue& out);

/*
 * 在缓冲区中查找第一个完整 JSON 对象的结束位置。
 * 返回结束位置（闭合 } 之后），找不到返回 std::string::npos。
 */
size_t json_find_end(const std::string& buf, size_t start = 0);

#endif /* JSON_H */
