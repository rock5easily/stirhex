// CStirlingSettings のレジストリ永続化。近代レイアウト（原の値名/バイナリ構造は再現しない）。
//   CWinApp の Profile API（SetRegistryKey 済なら HKCU\Software\<key>\<app>\<section>）を使用。
#include "pch.h"
#include "app/StirlingSettings.h"

namespace {
// COLORREF は DWORD だが本アプリの色は全て 0x00FFFFFF 以下のため int で往復可能。
inline int  GetInt(LPCTSTR sec, LPCTSTR key, int def) {
    return AfxGetApp()->GetProfileInt(sec, key, def);
}
inline void PutInt(LPCTSTR sec, LPCTSTR key, int val) {
    AfxGetApp()->WriteProfileInt(sec, key, val);
}
}

// データ種別索引→文字色フィールド参照（0..7）。
COLORREF& CStirlingSettings::CategoryText(int i) {
    switch (i) {
    case 0:  return headerText;
    case 1:  return addrText;
    case 2:  return dataText;
    case 3:  return markText[0];
    case 4:  return markText[1];
    case 5:  return markText[2];
    case 6:  return compareText;
    default: return structText;   // 7
    }
}
COLORREF& CStirlingSettings::CategoryBack(int i) {
    switch (i) {
    case 0:  return headerBack;
    case 1:  return addrBack;
    case 2:  return dataBack;
    case 3:  return markBack[0];
    case 4:  return markBack[1];
    case 5:  return markBack[2];
    case 6:  return compareBack;
    default: return structBack;   // 7
    }
}

// バイト値別色表（256）を構築（原 FUN_00436aaa）。全バイト= dataText、強調コードで上書き。
void CStirlingSettings::BuildByteColorTable(COLORREF table[256]) const {
    for (int i = 0; i < 256; ++i) { table[i] = dataText; }
    for (const auto& hc : hiCodes) { table[hc.first] = hc.second; }
}

void CStirlingSettings::Load(LPCTSTR sec) {
    // --- 表示状態（ページ158） ---
    lineSize        = GetInt(sec, _T("LineSize"),      lineSize);
    if (lineSize < 2)   { lineSize = 2; }
    if (lineSize > 256) { lineSize = 256; }
    addressBase     = GetInt(sec, _T("AddressBase"),   addressBase) ? 1 : 0;
    defCharset      = GetInt(sec, _T("CharSet"),       defCharset);
    if (defCharset < 0 || defCharset > 6) { defCharset = 1; }
    defByteOrderBig = GetInt(sec, _T("ByteOrder"),     defByteOrderBig) ? 1 : 0;
    addrHScroll     = GetInt(sec, _T("AddrHScroll"),   addrHScroll ? 1 : 0) != 0;
    openReadOnly    = GetInt(sec, _T("OpenReadOnly"),  openReadOnly ? 1 : 0) != 0;
    openInsertMode  = GetInt(sec, _T("OpenInsert"),    openInsertMode ? 1 : 0) != 0;
    openCharMode    = GetInt(sec, _T("OpenCharMode"),  openCharMode ? 1 : 0) != 0;
    fontHeight      = GetInt(sec, _T("FontHeight"),    fontHeight);
    fontWeight      = GetInt(sec, _T("FontWeight"),    fontWeight);
    fontItalic      = GetInt(sec, _T("FontItalic"),    fontItalic);
    fontFace = AfxGetApp()->GetProfileString(sec, _T("FontFace"), fontFace);

    // --- 色（ページ107） ---
    headerText  = (COLORREF)GetInt(sec, _T("HeaderText"),  (int)headerText);
    headerBack  = (COLORREF)GetInt(sec, _T("HeaderBack"),  (int)headerBack);
    addrText    = (COLORREF)GetInt(sec, _T("AddressText"), (int)addrText);
    addrBack    = (COLORREF)GetInt(sec, _T("AddressBack"), (int)addrBack);
    dataText    = (COLORREF)GetInt(sec, _T("DataText"),    (int)dataText);
    dataBack    = (COLORREF)GetInt(sec, _T("DataBack"),    (int)dataBack);
    compareText = (COLORREF)GetInt(sec, _T("CompareText"), (int)compareText);
    compareBack = (COLORREF)GetInt(sec, _T("CompareBack"), (int)compareBack);
    structText  = (COLORREF)GetInt(sec, _T("StructText"),  (int)structText);
    structBack  = (COLORREF)GetInt(sec, _T("StructBack"),  (int)structBack);
    for (int i = 0; i < 3; ++i) {
        CString kt, kb;
        kt.Format(_T("Mark%dText"), i + 1);
        kb.Format(_T("Mark%dBack"), i + 1);
        markText[i] = (COLORREF)GetInt(sec, kt, (int)markText[i]);
        markBack[i] = (COLORREF)GetInt(sec, kb, (int)markBack[i]);
    }

    // --- 強調表示コード＋ビットイメージ反映（ページ107） ---
    bimgReflect = GetInt(sec, _T("BitImageReflect"), bimgReflect ? 1 : 0) != 0;
    hiCodes.clear();
    const int hiCount = GetInt(sec, _T("HiCodeCount"), 0);
    for (int i = 0; i < hiCount; ++i) {
        CString kc, kk;
        kc.Format(_T("HiCode%d"), i);
        kk.Format(_T("HiColor%d"), i);
        const int code = GetInt(sec, kc, -1);
        if (code < 0 || code > 255) { continue; }
        const COLORREF color = (COLORREF)GetInt(sec, kk, 0);
        hiCodes.emplace_back((BYTE)code, color);
    }
}

void CStirlingSettings::Save(LPCTSTR sec) const {
    // --- 表示状態 ---
    PutInt(sec, _T("LineSize"),     lineSize);
    PutInt(sec, _T("AddressBase"),  addressBase);
    PutInt(sec, _T("CharSet"),      defCharset);
    PutInt(sec, _T("ByteOrder"),    defByteOrderBig);
    PutInt(sec, _T("AddrHScroll"),  addrHScroll ? 1 : 0);
    PutInt(sec, _T("OpenReadOnly"), openReadOnly ? 1 : 0);
    PutInt(sec, _T("OpenInsert"),   openInsertMode ? 1 : 0);
    PutInt(sec, _T("OpenCharMode"), openCharMode ? 1 : 0);
    PutInt(sec, _T("FontHeight"),   fontHeight);
    PutInt(sec, _T("FontWeight"),   fontWeight);
    PutInt(sec, _T("FontItalic"),   fontItalic);
    AfxGetApp()->WriteProfileString(sec, _T("FontFace"), fontFace);

    // --- 色 ---
    PutInt(sec, _T("HeaderText"),  (int)headerText);
    PutInt(sec, _T("HeaderBack"),  (int)headerBack);
    PutInt(sec, _T("AddressText"), (int)addrText);
    PutInt(sec, _T("AddressBack"), (int)addrBack);
    PutInt(sec, _T("DataText"),    (int)dataText);
    PutInt(sec, _T("DataBack"),    (int)dataBack);
    PutInt(sec, _T("CompareText"), (int)compareText);
    PutInt(sec, _T("CompareBack"), (int)compareBack);
    PutInt(sec, _T("StructText"),  (int)structText);
    PutInt(sec, _T("StructBack"),  (int)structBack);
    for (int i = 0; i < 3; ++i) {
        CString kt, kb;
        kt.Format(_T("Mark%dText"), i + 1);
        kb.Format(_T("Mark%dBack"), i + 1);
        PutInt(sec, kt, (int)markText[i]);
        PutInt(sec, kb, (int)markBack[i]);
    }

    // --- 強調表示コード＋ビットイメージ反映 ---
    PutInt(sec, _T("BitImageReflect"), bimgReflect ? 1 : 0);
    PutInt(sec, _T("HiCodeCount"), (int)hiCodes.size());
    for (int i = 0; i < (int)hiCodes.size(); ++i) {
        CString kc, kk;
        kc.Format(_T("HiCode%d"), i);
        kk.Format(_T("HiColor%d"), i);
        PutInt(sec, kc, (int)hiCodes[i].first);
        PutInt(sec, kk, (int)hiCodes[i].second);
    }
}
