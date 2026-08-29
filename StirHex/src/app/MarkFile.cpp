// MarkFile 実装（MarkFile.h 参照）。
#include "app/MarkFile.h"

#include <cstdio>
#include <cwchar>

#include "app/SettingsStore.h"

namespace stirling {
namespace marks {

namespace {

const wchar_t kSectionHeader[] = L"Mark";
const wchar_t kSectionMarks[]  = L"Marks";
const wchar_t kKeyVersion[]    = L"Version";
const wchar_t kKeyFile[]       = L"File";
const wchar_t kKeySize[]       = L"Size";

// 現在の形式版。読み込み時にこれ以外を拒否するのは、将来キー構成を変えたときに
//   古い版が新しいファイルを黙って部分適用しないようにするため。
const int kFormatVersion = 1;

std::wstring ToHex(stirling::FileOffset value) {
    wchar_t buf[32] = {0};
    std::swprintf(buf, 32, L"%llX", static_cast<unsigned long long>(value));
    return std::wstring(buf);
}

std::wstring ToDec(long long value) {
    wchar_t buf[32] = {0};
    std::swprintf(buf, 32, L"%lld", value);
    return std::wstring(buf);
}

// 16進文字列 → 位置。接頭辞は受け付けない（キーはアドレスそのもの）。
bool ParseHexOffset(const std::wstring& text, stirling::FileOffset* out) {
    if (text.empty() || text.size() > 16) { return false; }
    unsigned long long value = 0;
    for (const wchar_t c : text) {
        int digit = -1;
        if (c >= L'0' && c <= L'9') { digit = c - L'0'; }
        else if (c >= L'a' && c <= L'f') { digit = c - L'a' + 10; }
        else if (c >= L'A' && c <= L'F') { digit = c - L'A' + 10; }
        else { return false; }
        value = (value << 4) | static_cast<unsigned long long>(digit);
    }
    // 16桁までしか受け付けないため、符号ビットに食い込む値だけを弾けばよい。
    if (value > 0x7FFFFFFFFFFFFFFFull) { return false; }
    *out = static_cast<stirling::FileOffset>(value);
    return true;
}

bool ParseDecimal(const std::wstring& text, long long* out) {
    if (text.empty()) { return false; }
    size_t i = 0;
    bool negative = false;
    if (text[0] == L'-') { negative = true; i = 1; }
    if (i >= text.size()) { return false; }
    // 乗算・加算の前に上限を検査する。あふれてから符号を見ると、符号付き整数の
    //   オーバーフロー自体が未定義動作になる（MSVC ではラップして範囲検査を
    //   すり抜け、不正なファイルが受理されうる。Issue #132）。
    const long long kMax = 0x7FFFFFFFFFFFFFFFll;   // LLONG_MAX
    long long value = 0;
    for (; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (c < L'0' || c > L'9') { return false; }
        const int digit = c - L'0';
        if (value > (kMax - digit) / 10) { return false; }   // 桁あふれ
        value = value * 10 + digit;
    }
    *out = negative ? -value : value;
    return true;
}

std::wstring Quoted(const std::wstring& text) {
    return L"'" + text + L"'";
}

}  // namespace

std::wstring SerializeMarks(const MarkFileData& data) {
    settings::SettingsStore store;
    store.Set(kSectionHeader, kKeyVersion, ToDec(kFormatVersion));
    if (!data.sourcePath.empty()) {
        store.Set(kSectionHeader, kKeyFile, data.sourcePath);
    }
    if (data.sourceSize >= 0) {
        store.Set(kSectionHeader, kKeySize, ToDec(static_cast<long long>(data.sourceSize)));
    }
    // std::map なので位置の昇順。手で読むときにも差分を取るときにも都合がよい。
    for (const auto& kv : data.marks) {
        store.Set(kSectionMarks, ToHex(kv.first), ToDec(kv.second));
    }

    // マークが 0 件でもセクション行だけは書いておく（人が開いたときに、書き出しに
    //   失敗したのではなく 0 件だったと分かる）。読み込み側はこの行に依存しない。
    std::wstring text = L"; StirHex mark file\n" + store.Serialize();
    if (data.marks.empty()) {
        text += L"[";
        text += kSectionMarks;
        text += L"]\n";
    }
    return text;
}

bool ParseMarks(const std::wstring& text, MarkFileData& out, std::wstring& error) {
    error.clear();

    settings::SettingsStore store;
    if (!store.ParseInto(text)) {
        error = L"ファイルの形式が正しくありません";
        return false;
    }

    const std::wstring* version = store.Find(kSectionHeader, kKeyVersion);
    if (version == nullptr) {
        error = L"マークファイルではありません（[Mark] の Version がありません）";
        return false;
    }
    long long versionValue = 0;
    if (!ParseDecimal(*version, &versionValue) || versionValue != kFormatVersion) {
        error = L"対応していない形式です（Version=" + *version + L"）";
        return false;
    }

    const settings::SettingsStore::Section* marksSection = nullptr;
    for (const auto& section : store.Sections()) {
        // セクション名の比較はストアと同じく ASCII の大文字小文字を無視する。
        if (section.name.size() != std::wcslen(kSectionMarks)) { continue; }
        bool same = true;
        for (size_t i = 0; i < section.name.size(); ++i) {
            wchar_t a = section.name[i];
            wchar_t b = kSectionMarks[i];
            if (a >= L'A' && a <= L'Z') { a = static_cast<wchar_t>(a - L'A' + L'a'); }
            if (b >= L'A' && b <= L'Z') { b = static_cast<wchar_t>(b - L'A' + L'a'); }
            if (a != b) { same = false; break; }
        }
        if (same) { marksSection = &section; break; }
    }
    // 全体を解釈できてから差し替える（途中まで適用された状態を作らない）。
    MarkFileData parsed;
    // [Marks] が無い場合はマーク 0 件として扱う。ストアは値を持たないセクションを
    //   保持しないため、0 件のファイルを読み直すとこの状態になる。ファイルの識別は
    //   [Mark] の Version で足りている。
    static const std::vector<settings::SettingsStore::Entry> kNoEntries;
    const auto& entries = (marksSection != nullptr) ? marksSection->entries : kNoEntries;
    for (const auto& entry : entries) {
        stirling::FileOffset pos = 0;
        if (!ParseHexOffset(entry.key, &pos)) {
            error = L"アドレスとして読めません: " + Quoted(entry.key);
            return false;
        }
        long long type = 0;
        if (!ParseDecimal(entry.value, &type) ||
            type < kMinMarkNumber || type > kMaxMarkNumber) {
            error = L"マークの種別が 1〜3 ではありません: " + Quoted(entry.key) +
                    L" = " + Quoted(entry.value);
            return false;
        }
        parsed.marks[pos] = static_cast<int>(type);
    }

    if (const std::wstring* path = store.Find(kSectionHeader, kKeyFile)) {
        parsed.sourcePath = *path;
    }
    if (const std::wstring* size = store.Find(kSectionHeader, kKeySize)) {
        long long sizeValue = 0;
        // サイズは情報でしかないため、読めなければ「不明」に倒して読み込み自体は続ける。
        parsed.sourceSize = ParseDecimal(*size, &sizeValue) && sizeValue >= 0
                                ? static_cast<stirling::FileOffset>(sizeValue)
                                : -1;
    }

    out = parsed;
    return true;
}

std::wstring EncodeMarkList(const std::map<stirling::FileOffset, int>& marks) {
    std::wstring text;
    size_t written = 0;
    for (const auto& kv : marks) {
        if (written >= kMaxStoredMarks) { break; }
        if (kv.second < kMinMarkNumber || kv.second > kMaxMarkNumber) { continue; }
        if (!text.empty()) { text += L','; }
        text += ToHex(kv.first);
        text += L':';
        text += ToDec(kv.second);
        ++written;
    }
    return text;
}

bool DecodeMarkList(const std::wstring& text, std::map<stirling::FileOffset, int>& out) {
    std::map<stirling::FileOffset, int> parsed;
    size_t i = 0;
    while (i < text.size()) {
        const size_t comma = text.find(L',', i);
        const std::wstring item = text.substr(i, (comma == std::wstring::npos)
                                                     ? std::wstring::npos : comma - i);
        i = (comma == std::wstring::npos) ? text.size() : comma + 1;
        if (item.empty()) { continue; }   // 区切りが続いた場合は空要素として読み飛ばす

        const size_t colon = item.find(L':');
        if (colon == std::wstring::npos) { return false; }
        stirling::FileOffset pos = 0;
        if (!ParseHexOffset(item.substr(0, colon), &pos)) { return false; }
        long long type = 0;
        if (!ParseDecimal(item.substr(colon + 1), &type) ||
            type < kMinMarkNumber || type > kMaxMarkNumber) {
            return false;
        }
        parsed[pos] = static_cast<int>(type);
    }
    out = parsed;
    return true;
}

}  // namespace marks
}  // namespace stirling
