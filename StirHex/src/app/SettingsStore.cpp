// SettingsStore 実装。MFC / Win32 に依存しない（app/SettingsStore.h の規約を参照）。
#include "app/SettingsStore.h"

#include <cstddef>

namespace stirling {
namespace settings {

namespace {

const wchar_t kReplacementChar = 0xFFFD;

bool IsAsciiSpace(wchar_t c) {
    return c == L' ' || c == L'\t';
}

wchar_t ToLowerAscii(wchar_t c) {
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
}

// セクション名・キー名の比較（ASCII 範囲のみ大文字小文字を無視する）。
bool EqualsNoCaseAscii(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) { return false; }
    for (size_t i = 0; i < a.size(); ++i) {
        if (ToLowerAscii(a[i]) != ToLowerAscii(b[i])) { return false; }
    }
    return true;
}

std::wstring TrimAscii(const std::wstring& text) {
    size_t first = 0;
    while (first < text.size() && IsAsciiSpace(text[first])) { ++first; }
    size_t last = text.size();
    while (last > first && IsAsciiSpace(text[last - 1])) { --last; }
    return text.substr(first, last - first);
}

bool IsControl(wchar_t c) {
    return c < 0x20 || c == 0x7F;
}

int HexDigit(wchar_t c) {
    if (c >= L'0' && c <= L'9') { return static_cast<int>(c - L'0'); }
    if (c >= L'a' && c <= L'f') { return static_cast<int>(c - L'a') + 10; }
    if (c >= L'A' && c <= L'F') { return static_cast<int>(c - L'A') + 10; }
    return -1;
}

void AppendHex4(std::wstring& out, unsigned value) {
    const wchar_t* digits = L"0123456789ABCDEF";
    out.push_back(digits[(value >> 12) & 0xF]);
    out.push_back(digits[(value >> 8) & 0xF]);
    out.push_back(digits[(value >> 4) & 0xF]);
    out.push_back(digits[value & 0xF]);
}

void AppendUtf8CodePoint(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

void AppendWideCodePoint(std::wstring& out, unsigned cp) {
    if (cp < 0x10000) {
        out.push_back(static_cast<wchar_t>(cp));
    } else {
        const unsigned v = cp - 0x10000;
        out.push_back(static_cast<wchar_t>(0xD800 + (v >> 10)));
        out.push_back(static_cast<wchar_t>(0xDC00 + (v & 0x3FF)));
    }
}

}  // namespace

std::string WideToUtf8(const std::wstring& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        unsigned cp = static_cast<unsigned>(static_cast<unsigned short>(text[i]));
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            // 上位サロゲート。対になる下位サロゲートが続く場合だけ結合する。
            const unsigned low = (i + 1 < text.size())
                ? static_cast<unsigned>(static_cast<unsigned short>(text[i + 1]))
                : 0u;
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            } else {
                cp = kReplacementChar;   // 対にならない上位サロゲート
            }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            cp = kReplacementChar;       // 単独の下位サロゲート
        }
        AppendUtf8CodePoint(out, cp);
    }
    return out;
}

bool Utf8ToWide(const std::string& text, std::wstring& out) {
    out.clear();
    out.reserve(text.size());
    bool valid = true;
    size_t i = 0;
    while (i < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[i]);
        unsigned cp = 0;
        size_t extra = 0;
        unsigned lowest = 0;
        if (lead < 0x80) {
            cp = lead; extra = 0; lowest = 0;
        } else if ((lead & 0xE0) == 0xC0) {
            cp = lead & 0x1Fu; extra = 1; lowest = 0x80;
        } else if ((lead & 0xF0) == 0xE0) {
            cp = lead & 0x0Fu; extra = 2; lowest = 0x800;
        } else if ((lead & 0xF8) == 0xF0) {
            cp = lead & 0x07u; extra = 3; lowest = 0x10000;
        } else {
            out.push_back(kReplacementChar); valid = false; ++i; continue;
        }
        if (i + extra >= text.size()) {
            out.push_back(kReplacementChar); valid = false; ++i; continue;
        }
        bool ok = true;
        for (size_t k = 1; k <= extra; ++k) {
            const unsigned char cont = static_cast<unsigned char>(text[i + k]);
            if ((cont & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cont & 0x3Fu);
        }
        // 冗長符号化・サロゲート域・範囲外は不正として扱う。
        if (!ok || cp < lowest || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            out.push_back(kReplacementChar); valid = false; ++i; continue;
        }
        AppendWideCodePoint(out, cp);
        i += extra + 1;
    }
    return valid;
}

std::wstring BytesToHex(const unsigned char* data, size_t size) {
    std::wstring out;
    if (data == nullptr) { return out; }
    const wchar_t* digits = L"0123456789ABCDEF";
    out.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        out.push_back(digits[(data[i] >> 4) & 0xF]);
        out.push_back(digits[data[i] & 0xF]);
    }
    return out;
}

bool HexToBytes(const std::wstring& text, std::vector<unsigned char>& out) {
    if ((text.size() % 2) != 0) { return false; }
    std::vector<unsigned char> parsed;
    parsed.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        const int hi = HexDigit(text[i]);
        const int lo = HexDigit(text[i + 1]);
        if (hi < 0 || lo < 0) { return false; }
        parsed.push_back(static_cast<unsigned char>((hi << 4) | lo));
    }
    out.swap(parsed);
    return true;
}

// 値のエスケープ。素のまま書けない値だけを引用符で囲む（可読性を保つため）。
std::wstring EncodeValue(const std::wstring& value) {
    bool needQuote = false;
    if (!value.empty()) {
        if (IsAsciiSpace(value.front()) || IsAsciiSpace(value.back()) || value.front() == L'"') {
            needQuote = true;
        }
    }
    if (!needQuote) {
        for (wchar_t c : value) {
            if (IsControl(c)) { needQuote = true; break; }
        }
    }
    if (!needQuote) { return value; }

    std::wstring out;
    out.reserve(value.size() + 2);
    out.push_back(L'"');
    for (wchar_t c : value) {
        switch (c) {
            case L'\\': out += L"\\\\"; break;
            case L'"':  out += L"\\\""; break;
            case L'\r': out += L"\\r";  break;
            case L'\n': out += L"\\n";  break;
            case L'\t': out += L"\\t";  break;
            default:
                if (IsControl(c)) {
                    out += L"\\x";
                    AppendHex4(out, static_cast<unsigned>(c));
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    out.push_back(L'"');
    return out;
}

std::wstring DecodeValue(const std::wstring& text) {
    // 引用符で囲まれていなければそのままの値（`C:\path` を素直に読むため）。
    if (text.size() < 2 || text.front() != L'"' || text.back() != L'"') {
        return text;
    }
    std::wstring out;
    out.reserve(text.size());
    const size_t bodyEnd = text.size() - 1;   // 閉じ引用符の位置
    for (size_t i = 1; i < bodyEnd; ++i) {
        if (text[i] != L'\\') { out.push_back(text[i]); continue; }
        // 末尾に取り残されたエスケープ文字はそのまま値に残す。
        if (i + 1 >= bodyEnd) { out.push_back(L'\\'); break; }
        const wchar_t esc = text[++i];
        switch (esc) {
            case L'\\': out.push_back(L'\\'); break;
            case L'"':  out.push_back(L'"');  break;
            case L'r':  out.push_back(L'\r'); break;
            case L'n':  out.push_back(L'\n'); break;
            case L't':  out.push_back(L'\t'); break;
            case L'x': {
                unsigned v = 0;
                int digits = 0;
                while (digits < 4 && i + 1 < bodyEnd) {
                    const int d = HexDigit(text[i + 1]);
                    if (d < 0) { break; }
                    v = (v << 4) | static_cast<unsigned>(d);
                    ++i; ++digits;
                }
                if (digits == 0) { out += L"\\x"; } else { out.push_back(static_cast<wchar_t>(v)); }
                break;
            }
            default:
                // 未知のエスケープは元の2文字をそのまま残す（値を勝手に壊さない）。
                out.push_back(L'\\');
                out.push_back(esc);
                break;
        }
    }
    return out;
}

SettingsStore::Section* SettingsStore::FindSection(const std::wstring& name) {
    for (Section& s : sections_) {
        if (EqualsNoCaseAscii(s.name, name)) { return &s; }
    }
    return nullptr;
}

const SettingsStore::Section* SettingsStore::FindSection(const std::wstring& name) const {
    for (const Section& s : sections_) {
        if (EqualsNoCaseAscii(s.name, name)) { return &s; }
    }
    return nullptr;
}

const std::wstring* SettingsStore::Find(const std::wstring& section, const std::wstring& key) const {
    const Section* s = FindSection(section);
    if (s == nullptr) { return nullptr; }
    for (const Entry& e : s->entries) {
        if (EqualsNoCaseAscii(e.key, key)) { return &e.value; }
    }
    return nullptr;
}

void SettingsStore::Set(const std::wstring& section, const std::wstring& key,
                        const std::wstring& value) {
    Section* s = FindSection(section);
    if (s == nullptr) {
        sections_.push_back(Section{ section, {} });
        s = &sections_.back();
    }
    for (Entry& e : s->entries) {
        if (EqualsNoCaseAscii(e.key, key)) {
            if (e.value != value) {
                e.value = value;
                MarkChanged(Change{ Change::Kind::Set, section, key, value });
            }
            return;
        }
    }
    s->entries.push_back(Entry{ key, value });
    MarkChanged(Change{ Change::Kind::Set, section, key, value });
}

void SettingsStore::Remove(const std::wstring& section, const std::wstring& key) {
    Section* s = FindSection(section);
    if (s == nullptr) { return; }
    for (size_t i = 0; i < s->entries.size(); ++i) {
        if (EqualsNoCaseAscii(s->entries[i].key, key)) {
            s->entries.erase(s->entries.begin() + static_cast<std::ptrdiff_t>(i));
            MarkChanged(Change{ Change::Kind::Remove, section, key, std::wstring() });
            return;
        }
    }
}

void SettingsStore::RemoveSection(const std::wstring& section) {
    for (size_t i = 0; i < sections_.size(); ++i) {
        if (EqualsNoCaseAscii(sections_[i].name, section)) {
            sections_.erase(sections_.begin() + static_cast<std::ptrdiff_t>(i));
            MarkChanged(Change{ Change::Kind::RemoveSection, section,
                                std::wstring(), std::wstring() });
            return;
        }
    }
}

void SettingsStore::Clear() {
    if (!sections_.empty()) {
        MarkChanged(Change{ Change::Kind::Clear, std::wstring(),
                            std::wstring(), std::wstring() });
    }
    sections_.clear();
}

// 変更の記録。ParseInto（読み込み）の間だけ記録を止める。
void SettingsStore::MarkChanged(const Change& change) {
    dirty_ = true;
    if (recording_) { changes_.push_back(change); }
}

void SettingsStore::ApplyChanges(const std::vector<Change>& changes) {
    for (const Change& c : changes) {
        switch (c.kind) {
        case Change::Kind::Set:           Set(c.section, c.key, c.value); break;
        case Change::Kind::Remove:        Remove(c.section, c.key); break;
        case Change::Kind::RemoveSection: RemoveSection(c.section); break;
        case Change::Kind::Clear:         Clear(); break;
        }
    }
}

std::wstring SettingsStore::Serialize() const {
    std::wstring out;
    for (const Section& s : sections_) {
        if (!out.empty()) { out += L"\r\n"; }
        out += L"[";
        out += s.name;
        out += L"]\r\n";
        for (const Entry& e : s.entries) {
            out += e.key;
            out += L"=";
            out += EncodeValue(e.value);
            out += L"\r\n";
        }
    }
    return out;
}

bool SettingsStore::ParseInto(const std::wstring& text) {
    bool clean = true;
    std::wstring current;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) { end = text.size(); }
        std::wstring line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == L'\r') { line.pop_back(); }

        const std::wstring trimmed = TrimAscii(line);
        if (trimmed.empty() || trimmed[0] == L';' || trimmed[0] == L'#') { continue; }
        if (trimmed[0] == L'[') {
            if (trimmed.size() >= 2 && trimmed.back() == L']') {
                current = TrimAscii(trimmed.substr(1, trimmed.size() - 2));
            } else {
                clean = false;   // 閉じ括弧の無いセクション行
            }
            continue;
        }
        const size_t eq = trimmed.find(L'=');
        if (eq == std::wstring::npos) {
            clean = false;       // `=` の無い行
            continue;
        }
        const std::wstring key = TrimAscii(trimmed.substr(0, eq));
        if (key.empty()) {
            clean = false;
            continue;
        }
        // 値の前後の空白は落とす。前後に空白を持たせたい値は引用符付きで書かれているため
        //   失われない（手で編集した `Key = 7` のような行も素直に読める）。
        const std::wstring raw = TrimAscii(trimmed.substr(eq + 1));
        const bool wasDirty = dirty_;
        recording_ = false;      // 読み込みは「変更」ではない（記録も残さない）
        Set(current, key, DecodeValue(raw));
        recording_ = true;
        dirty_ = wasDirty;
    }
    return clean;
}

}  // namespace settings
}  // namespace stirling
