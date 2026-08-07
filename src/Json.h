#pragma once

// A deliberately small JSON reader and writer.
//
// It accepts the subset this plugin emits and rejects everything else with a
// message: a file that half loads would leave a setup in a state that never
// existed, which is harder to notice and harder to undo than an outright
// refusal.
//
// Shared by presets and by the preferences file rather than written twice. Two
// parsers would drift, and the second one is always the one without the tests.

#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>

namespace mtx {
namespace json {

/** @brief Format a number for output.
 *
 * %.6g keeps the files readable and round-trips a float exactly, without
 * emitting the long decimal tails that make a diff unreadable.
 */
inline std::string Num(double v)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6g", v);
    return buf;
}

inline std::string Escape(const std::string& s)
{
    std::string out;
    for (char ch : s)
    {
        switch (ch)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                // Control characters would produce invalid JSON; drop them
                // rather than emitting a file we cannot read back.
                if (static_cast<unsigned char>(ch) >= 0x20) out += ch;
                break;
        }
    }
    return out;
}

struct Reader
{
    const std::string& s;
    size_t i = 0;
    std::string err;

    explicit Reader(const std::string& src) : s(src) {}

    void skipWs() { while (i < s.size() && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) ++i; }
    bool eof()    { skipWs(); return i >= s.size(); }
    char peek()   { skipWs(); return i < s.size() ? s[i] : '\0'; }

    bool expect(char c)
    {
        skipWs();
        if (i < s.size() && s[i] == c) { ++i; return true; }
        err = std::string("expected '") + c + "' at offset " + std::to_string(i);
        return false;
    }

    bool readString(std::string& out)
    {
        skipWs();
        if (i >= s.size() || s[i] != '"') { err = "expected a string at offset " + std::to_string(i); return false; }
        ++i;
        out.clear();
        while (i < s.size() && s[i] != '"')
        {
            if (s[i] == '\\')
            {
                if (++i >= s.size()) { err = "truncated escape"; return false; }
                switch (s[i])
                {
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case '"': out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/';  break;
                    default: err = "unsupported escape"; return false;
                }
                ++i;
            }
            else out += s[i++];
        }
        if (i >= s.size()) { err = "unterminated string"; return false; }
        ++i;
        return true;
    }

    bool readNumber(double& out)
    {
        skipWs();
        const size_t start = i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
        bool digits = false;
        while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i]=='.' || s[i]=='e' || s[i]=='E'
                                || ((s[i]=='-'||s[i]=='+') && (s[i-1]=='e'||s[i-1]=='E'))))
        {
            if (s[i] >= '0' && s[i] <= '9') digits = true;
            ++i;
        }
        if (!digits) { err = "expected a number at offset " + std::to_string(start); return false; }

        out = std::strtod(s.substr(start, i - start).c_str(), nullptr);
        return true;
    }

    /** Skip one value of any type, so unknown keys do not break the parse. */
    bool skipValue()
    {
        skipWs();
        if (i >= s.size()) { err = "unexpected end of input"; return false; }

        const char c = s[i];
        if (c == '"') { std::string t; return readString(t); }
        if (c == '{' || c == '[')
        {
            const char close = (c == '{') ? '}' : ']';
            int depth = 0;
            while (i < s.size())
            {
                if (s[i] == '"') { std::string t; if (!readString(t)) return false; continue; }
                if (s[i] == c)     ++depth;
                if (s[i] == close) { if (--depth == 0) { ++i; return true; } }
                ++i;
            }
            err = "unterminated object or array";
            return false;
        }
        if (s.compare(i, 4, "true") == 0)  { i += 4; return true; }
        if (s.compare(i, 5, "false") == 0) { i += 5; return true; }
        if (s.compare(i, 4, "null") == 0)  { i += 4; return true; }

        double d = 0.0;
        return readNumber(d);
    }
};

/// One flat object of key -> raw scalar, which is all these schemas need.
using Fields = std::map<std::string, std::string>;

inline bool ReadFlatObject(Reader& r, Fields& out)
{
    if (!r.expect('{')) return false;
    if (r.peek() == '}') { ++r.i; return true; }

    for (;;)
    {
        std::string key;
        if (!r.readString(key)) return false;
        if (!r.expect(':')) return false;

        r.skipWs();
        const size_t valueStart = r.i;
        if (!r.skipValue()) return false;

        // Stored raw; conversion happens at the point of use so a wrong type is
        // caught with the key name rather than as a bare offset.
        out[key] = r.s.substr(valueStart, r.i - valueStart);

        r.skipWs();
        if (r.peek() == ',') { ++r.i; continue; }
        if (r.peek() == '}') { ++r.i; return true; }
        r.err = "expected ',' or '}' at offset " + std::to_string(r.i);
        return false;
    }
}

inline double FieldNum(const Fields& f, const char* key, double fallback)
{
    const auto it = f.find(key);
    if (it == f.end() || it->second.empty()) return fallback;
    if (it->second[0] == '"') return fallback;      // a string where a number belongs
    return std::strtod(it->second.c_str(), nullptr);
}

inline bool FieldBool(const Fields& f, const char* key, bool fallback)
{
    const auto it = f.find(key);
    if (it == f.end()) return fallback;
    if (it->second == "true")  return true;
    if (it->second == "false") return false;
    return fallback;
}

/** @brief Decode a raw JSON string token, quotes included, back to its text.
 *
 * Stripping the quotes is not enough: the stored token is the raw source, so a
 * value containing a quote or a tab would otherwise come back with its
 * backslash escapes intact and drift a little further every save/load cycle.
 */
inline std::string Unescape(const std::string& quoted)
{
    std::string out;
    for (size_t i = 1; i + 1 < quoted.size(); ++i)
    {
        if (quoted[i] != '\\') { out += quoted[i]; continue; }
        if (i + 2 >= quoted.size() + 1) break;

        switch (quoted[++i])
        {
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case '"':  out += '"';  break;
            case '\\': out += '\\'; break;
            case '/':  out += '/';  break;
            default:   out += quoted[i]; break;
        }
    }
    return out;
}

inline std::string FieldStr(const Fields& f, const char* key, const std::string& fallback)
{
    const auto it = f.find(key);
    if (it == f.end() || it->second.size() < 2 || it->second[0] != '"') return fallback;
    return Unescape(it->second);
}

} // namespace json
} // namespace mtx
