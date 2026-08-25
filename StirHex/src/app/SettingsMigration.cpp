// SettingsMigration 実装（仕様と背景は SettingsMigration.h を参照）。
#include "app/SettingsMigration.h"

#include <windows.h>

namespace stirling {
namespace settings {
namespace {

// MBCS 版が明示変換に使っていたコードページ（ToCp932 シム）。
constexpr UINT kCp932 = 932;

// ワイド → 指定コードページのバイト列。best-fit 置換が起きたら失敗扱い（欠落に気付けないため）。
bool NarrowExact(const std::wstring& w, UINT cp, std::string& out) {
    if (w.empty()) { out.clear(); return true; }
    const int len = static_cast<int>(w.size());
    BOOL usedDefault = FALSE;
    const int n = ::WideCharToMultiByte(cp, WC_NO_BEST_FIT_CHARS, w.c_str(), len,
                                        nullptr, 0, nullptr, &usedDefault);
    if (n <= 0) { return false; }

    std::string a(static_cast<size_t>(n), '\0');
    usedDefault = FALSE;
    const int got = ::WideCharToMultiByte(cp, WC_NO_BEST_FIT_CHARS, w.c_str(), len,
                                          &a[0], n, nullptr, &usedDefault);
    if (got <= 0 || usedDefault) { return false; }
    a.resize(static_cast<size_t>(got));
    out.swap(a);
    return true;
}

// 指定コードページのバイト列 → ワイド。不正なシーケンスは失敗扱い（黙って U+FFFD にしない）。
bool WidenExact(const std::string& a, UINT cp, std::wstring& out) {
    if (a.empty()) { out.clear(); return true; }
    const int len = static_cast<int>(a.size());
    const int n = ::MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, a.c_str(), len, nullptr, 0);
    if (n <= 0) { return false; }

    std::wstring w(static_cast<size_t>(n), L'\0');
    const int got = ::MultiByteToWideChar(cp, MB_ERR_INVALID_CHARS, a.c_str(), len, &w[0], n);
    if (got <= 0) { return false; }
    w.resize(static_cast<size_t>(got));
    out.swap(w);
    return true;
}

}  // namespace

bool RepairCp932ViaAcp(const std::wstring& stored, unsigned int acp, std::wstring& out) {
    const UINT cp = static_cast<UINT>(acp);

    // ACP=932: MBCS 版の CP932 バイト列はそのまま正しく UTF-16 化されている（変換が恒等）。
    if (cp == kCp932) { return false; }
    // UTF-8 / UTF-7: 書き込み時に CP932 バイト列が不正シーケンスとして U+FFFD へ潰れており、
    //   複数バイトが 3 バイト固定の置換文字に化けているため、バイト境界ごと復元できない。
    //   実測では巻き戻した後のバイト列が CP932 として妥当でないと判定され、下の検証でも
    //   拒否される。ただしその判定に頼らず、復元対象外であることをここで明示する。
    if (cp == CP_UTF8 || cp == CP_UTF7) { return false; }
    if (stored.empty()) { return false; }

    // MBCS 版の書き込み（RegSetValueExA の ACP 変換）を巻き戻して CP932 バイト列を取り出す。
    std::string bytes;
    if (!NarrowExact(stored, cp, bytes)) { return false; }

    // 巻き戻しが可逆であることを確認する。ここが一致しない値は、そもそも ACP 変換で
    //   生じたものではない（Unicode ビルドが既に正しく書いた値など）。
    std::wstring verify;
    if (!WidenExact(bytes, cp, verify) || verify != stored) { return false; }

    // CP932 として妥当に読めたときだけ復元とみなす。
    std::wstring repaired;
    if (!WidenExact(bytes, kCp932, repaired)) { return false; }
    if (repaired == stored) { return false; }   // ASCII のみの値。書き戻す必要がない

    // ここまでの検証は「ACP 変換が可逆」「CP932 として妥当」を示すだけで、その値が
    //   本当に CP932 バイト列由来だったことは示さない。単バイトだけで読めてしまう値は
    //   正しい値を壊す側へ倒れやすいので拒否する。
    //   例: ACP=1252 で Unicode ビルドが正しく書いた L"C:\\\u00A5"（C:\¥）は、CP1252 の
    //   バイト列 43 3A 5C A5 になり、0xA5 は CP932 の半角カナ U+FF65 としても妥当に
    //   読めてしまう。一方 MBCS 版が ToCp932 を通した「化ける値」は必ず全角＝2バイト
    //   シーケンスを含む（ASCII と半角カナだけの値は ACP 変換でも化けない）。
    //   CP932 は BMP 内に閉じており（サロゲートを生まない）、2バイトシーケンスを1つでも
    //   消費していれば バイト数 > 文字数 になる。これを CP932 由来の必要条件として使う。
    if (bytes.size() <= repaired.size()) { return false; }

    out.swap(repaired);
    return true;
}

}  // namespace settings
}  // namespace stirling
