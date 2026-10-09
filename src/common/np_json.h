// 极简 JSON：仅用于保存/读取 NextPerf 的配置，不追求完整规范覆盖，
// 但对 "\" 转义、UTF-8、数字、对象、数组都做了正确处理。
#pragma once

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <cstdio>
#include <cmath>

namespace np {

class Json {
public:
    enum Type { Null, Bool, Num, Str, Arr, Obj };

    struct Value {
        Type type = Null;
        bool b = false;
        double num = 0;
        std::string str;
        std::vector<Value> arr;
        std::map<std::string, Value> obj;

        bool isNull() const { return type == Null; }
        Value* find(const std::string& k) {
            auto it = obj.find(k);
            return it == obj.end() ? nullptr : &it->second;
        }
        const Value* find(const std::string& k) const {
            auto it = obj.find(k);
            return it == obj.end() ? nullptr : &it->second;
        }
        double numOr(double d) const { return type == Num ? num : d; }
        bool boolOr(bool d) const { return type == Bool ? b : d; }
        std::string strOr(const std::string& d) const { return type == Str ? str : d; }
    };

    // ---------------- 序列化 ----------------
    static std::string dump(const Value& v, int indent = 0) {
        std::string pad((size_t)indent * 2, ' ');
        switch (v.type) {
            case Null: return "null";
            case Bool: return v.b ? "true" : "false";
            case Num: {
                char buf[64];
                if (v.num == std::floor(v.num) && std::fabs(v.num) < 1e15)
                    snprintf(buf, sizeof(buf), "%.0f", v.num);
                else
                    snprintf(buf, sizeof(buf), "%.6g", v.num);
                return buf;
            }
            case Str: return quote(v.str);
            case Arr: {
                if (v.arr.empty()) return "[]";
                std::string s = "[\n";
                for (size_t i = 0; i < v.arr.size(); ++i) {
                    s += std::string((size_t)(indent + 1) * 2, ' ');
                    s += dump(v.arr[i], indent + 1);
                    if (i + 1 < v.arr.size()) s += ",";
                    s += "\n";
                }
                s += pad + "]";
                return s;
            }
            case Obj: {
                if (v.obj.empty()) return "{}";
                std::string s = "{\n";
                size_t i = 0;
                for (auto& kv : v.obj) {
                    s += std::string((size_t)(indent + 1) * 2, ' ');
                    s += quote(kv.first) + ": " + dump(kv.second, indent + 1);
                    if (++i < v.obj.size()) s += ",";
                    s += "\n";
                }
                s += pad + "}";
                return s;
            }
        }
        return "null";
    }

    // ---------------- 解析 ----------------
    struct Parser {
        const char* p;
        const char* end;
        bool ok = true;

        void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
        bool lit(const char* s) {
            size_t n = strlen(s);
            if ((size_t)(end - p) >= n && memcmp(p, s, n) == 0) { p += n; return true; }
            return false;
        }
        Value parse() {
            ws();
            Value v;
            if (p >= end) { ok = false; return v; }
            char c = *p;
            if (c == '{') return parseObj();
            if (c == '[') return parseArr();
            if (c == '"') { v.type = Str; v.str = parseStr(); return v; }
            if (lit("true")) { v.type = Bool; v.b = true; return v; }
            if (lit("false")) { v.type = Bool; v.b = false; return v; }
            if (lit("null")) { v.type = Null; return v; }
            return parseNum();
        }
        Value parseObj() {
            Value v; v.type = Obj; ++p; ws();
            if (p < end && *p == '}') { ++p; return v; }
            while (p < end) {
                ws();
                if (*p != '"') { ok = false; break; }
                std::string k = parseStr();
                ws();
                if (p < end && *p == ':') ++p; else { ok = false; break; }
                v.obj[k] = parse();
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; }
                break;
            }
            return v;
        }
        Value parseArr() {
            Value v; v.type = Arr; ++p; ws();
            if (p < end && *p == ']') { ++p; return v; }
            while (p < end) {
                v.arr.push_back(parse());
                ws();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; }
                break;
            }
            return v;
        }
        Value parseNum() {
            Value v; v.type = Num;
            const char* s = p;
            if (p < end && (*p == '-' || *p == '+')) ++p;
            while (p < end && (isdigit((unsigned char)*p) || *p == '.' || *p == 'e' || *p == 'E' ||
                               *p == '-' || *p == '+'))
                ++p;
            v.num = strtod(std::string(s, p).c_str(), nullptr);
            return v;
        }
        std::string parseStr() {
            std::string out;
            ++p;  // 跳过 "
            while (p < end && *p != '"') {
                if (*p == '\\' && p + 1 < end) {
                    ++p;
                    char e = *p++;
                    switch (e) {
                        case 'n': out += '\n'; break;
                        case 't': out += '\t'; break;
                        case 'r': out += '\r'; break;
                        case 'b': out += '\b'; break;
                        case 'f': out += '\f'; break;
                        case 'u': {
                            unsigned cp = 0;
                            for (int i = 0; i < 4 && p < end; ++i, ++p)
                                cp = cp * 16 + (unsigned)(isdigit((unsigned char)*p) ? *p - '0'
                                              : (tolower(*p) - 'a' + 10));
                            appendUtf8(out, cp);
                            break;
                        }
                        default: out += e;
                    }
                } else {
                    out += *p++;
                }
            }
            if (p < end) ++p;
            return out;
        }
    };

    static Value parse(const std::string& text, bool* okOut = nullptr) {
        Parser pr{text.c_str(), text.c_str() + text.size(), true};
        Value v = pr.parse();
        if (okOut) *okOut = pr.ok;
        return v;
    }

    // ---------------- 便捷构造 ----------------
    static Value mkNum(double d) { Value v; v.type = Num; v.num = d; return v; }
    static Value mkStr(const std::string& s) { Value v; v.type = Str; v.str = s; return v; }
    static Value mkBool(bool b) { Value v; v.type = Bool; v.b = b; return v; }
    static Value mkArr() { Value v; v.type = Arr; return v; }
    static Value mkObj() { Value v; v.type = Obj; return v; }

private:
    static std::string quote(const std::string& s) {
        std::string o = "\"";
        for (unsigned char c : s) {
            switch (c) {
                case '"': o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n"; break;
                case '\r': o += "\\r"; break;
                case '\t': o += "\\t"; break;
                default:
                    if (c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
                    else o += (char)c;
            }
        }
        return o + "\"";
    }
    static void appendUtf8(std::string& s, unsigned cp) {
        if (cp < 0x80) s += (char)cp;
        else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
        else { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    }
};

}  // namespace np
