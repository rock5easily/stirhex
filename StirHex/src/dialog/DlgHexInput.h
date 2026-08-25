// 検索/置換ダイアログ共通の16進入力ユーティリティ。
//   受理形式: (1) スペースで2桁区切り  (2) スペース無しの偶数桁。いずれも大文字/小文字可。
//   ParseHexStrict: 検証してバイト列へ（不正は false）。NormalizeHex: "XX XX" 大文字へ整形。
#pragma once

#include "app/UiStrings.h"

#include <vector>

namespace dlg {

// 文字列リソースの取得（実体は ui::LoadW。呼び出し側の互換のため別名を残す）。
inline CStringW LoadWStr(UINT id) { return ui::LoadW(id); }

// アドレス入力の厳密解析（原の判定に忠実。JumpDlg/MarkAddressDlg/RangeBarDlg/StructAddressDlg 共用）。
//   原の実測:
//     空／不正桁／前後に空白を含む → 基数別メッセージ（16進=1004 / 10進=1000）
//     桁あふれを含む範囲外の値     → 「指定アドレスは無効です」(1003)
//   したがって桁あふれは解析失敗とせず、必ず範囲判定で弾かれる値を返す。
const long long kAddrOverflow = 0x7FFFFFFFFFFFLL;   // 実データ長を必ず超える値

inline bool ParseAddrStrict(const CString& text, int base, long long& out) {
    if (text.IsEmpty()) { return false; }   // 空白のトリムはしない（原は空白を受け付けない）
    unsigned long long v = 0;
    bool overflow = false;
    for (int i = 0; i < text.GetLength(); ++i) {
        const TCHAR c = text[i];
        int d;
        if (c >= _T('0') && c <= _T('9')) { d = c - _T('0'); }
        else if (base == 16 && c >= _T('a') && c <= _T('f')) { d = c - _T('a') + 10; }
        else if (base == 16 && c >= _T('A') && c <= _T('F')) { d = c - _T('A') + 10; }
        else { return false; }
        if (d >= base) { return false; }
        if (v > (0xFFFFFFFFFFFFFFFFULL - static_cast<unsigned long long>(d)) /
                static_cast<unsigned long long>(base)) {
            overflow = true;   // 以降の桁は値に反映しない（範囲外として扱う）
        } else {
            v = v * static_cast<unsigned long long>(base) + static_cast<unsigned long long>(d);
        }
    }
    out = (overflow || v > static_cast<unsigned long long>(kAddrOverflow))
              ? kAddrOverflow : static_cast<long long>(v);
    return true;
}

// 16進1桁の値（ASCII 層。非 ASCII は -1）。
inline int HexVal(wchar_t c) {
    if (c >= L'0' && c <= L'9') { return c - L'0'; }
    if (c >= L'a' && c <= L'f') { return c - L'a' + 10; }
    if (c >= L'A' && c <= L'F') { return c - L'A' + 10; }
    return -1;
}

// 入口はワイド（入力欄は wide 層）。中身は 16 進表記のみを受理する ASCII 層。
inline bool ParseHexStrict(const CStringW& in, std::vector<unsigned char>& out) {
    out.clear();
    CStringW s = in;
    s.Trim(L" \t");
    if (s.IsEmpty()) { return false; }
    const int len = s.GetLength();

    if (s.Find(L' ') >= 0) {
        int i = 0;
        while (i < len) {
            while (i < len && s[i] == L' ') { ++i; }
            if (i >= len) { break; }
            const int start = i;
            while (i < len && s[i] != L' ') { ++i; }
            if (i - start != 2) { return false; }
            const int hi = HexVal(s[start]);
            const int lo = HexVal(s[start + 1]);
            if (hi < 0 || lo < 0) { return false; }
            out.push_back(static_cast<unsigned char>((hi << 4) | lo));
        }
    } else {
        if (len % 2 != 0) { return false; }
        for (int i = 0; i < len; i += 2) {
            const int hi = HexVal(s[i]);
            const int lo = HexVal(s[i + 1]);
            if (hi < 0 || lo < 0) { return false; }
            out.push_back(static_cast<unsigned char>((hi << 4) | lo));
        }
    }
    return !out.empty();
}

inline CStringW NormalizeHex(const std::vector<unsigned char>& bytes) {
    CStringW s;
    for (size_t i = 0; i < bytes.size(); ++i) {
        CStringW cell;
        cell.Format((i == 0) ? L"%02X" : L" %02X", bytes[i]);
        s += cell;
    }
    return s;
}

} // namespace dlg
