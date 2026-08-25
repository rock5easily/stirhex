// UI 文字列ユーティリティ（wide 層）。
//   UIに表示する文字列はリソース（STRINGTABLE）に置き、コードへ直書きしないための共通処理。
//   Unicode ビルドではリソースから読んだワイド文字列をそのまま UI へ渡す。
//   byte 層（CP932 バイト列）との変換が必要な箇所は core/Cp932Text.h を使うこと。
#pragma once

#include <afxwin.h>

namespace ui {

// 文字列リソースをワイドで取得する（未定義IDは空文字列）。
inline CStringW LoadW(UINT id) {
    wchar_t buf[512] = {0};
    const int n = ::LoadStringW(AfxGetResourceHandle(), id, buf, _countof(buf));
    return CStringW(buf, n);
}

// アプリ名（メッセージボックスの表題。原はすべて AfxMessageBox 既定＝アプリ名を使う）。
inline CStringW AppTitleW() { return LoadW(AFX_IDS_APP_TITLE); }

// メッセージボックス（表題はアプリ名で統一。原の全メッセージが "Stirling" 表題であることを実測済み）。
inline int MsgBox(HWND owner, LPCWSTR text, UINT type = MB_OK | MB_ICONEXCLAMATION) {
    return ::MessageBoxW(owner, (text != nullptr) ? text : L"", AppTitleW(), type);
}

// 本文を文字列リソースから取るメッセージボックス。
inline int MsgBoxRes(HWND owner, UINT strId, UINT type = MB_OK | MB_ICONEXCLAMATION) {
    return MsgBox(owner, LoadW(strId), type);
}

// 機能名（コマンド名）の文字列リソースID。
//   rawID = (カテゴリ<<8)|項目番号 に対し、原の文字列表は 5100 + カテゴリ*100 + 項目番号 に並ぶ
//   （全112項目で一致を確認済み。例 0x0001→5101「新規作成」/ 0x030E→5414「構造体編集」）。
inline UINT CommandNameStringId(UINT rawId) {
    const UINT cat = (rawId >> 8) & 0xFF;
    const UINT item = rawId & 0xFF;
    return 5100 + cat * 100 + item;
}

// rawID に対応する機能名（未定義は空文字列）。
inline CStringW CommandNameW(UINT rawId) { return LoadW(CommandNameStringId(rawId)); }

// カテゴリ名（原 文字列 4000-4007: ファイル系/カーソル移動系/…/その他）。
inline CStringW CommandCategoryNameW(int category) { return LoadW(4000 + category); }

}  // namespace ui
