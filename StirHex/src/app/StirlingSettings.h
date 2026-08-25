// CStirlingSettings — 表示設定モデル（原 CMainFrame 共有の設定オブジェクト view+0x248 の
//   表示関連サブセット）。移植ではレジストリに拡張子別レコードを保存し、文書パスから
//   対応する表示設定を解決して各文書・ビューへ反映する。
//   既定値は原 設定読込 FUN_0041f2a5 のフォールバック値に一致（COLORREF=0x00BBGGRR）。
#pragma once

#include <windows.h>

#include <utility>
#include <vector>

class CStirlingSettings {
public:
    // --- 3カラムの色（原 struct[2..0x11]。COLORREF=0x00BBGGRR の生値で保持） ---
    COLORREF headerText = 0x000000;   // 黒（HeaderText）
    COLORREF headerBack = 0x00FFFF;   // 黄（HeaderBack）
    COLORREF addrText   = 0xFFFFFF;   // 白（AddressText）
    COLORREF addrBack   = 0x808080;   // 灰（AddressBack）
    COLORREF dataText   = 0x000000;   // 黒（DataText）
    COLORREF dataBack   = 0xFFFFFF;   // 白（DataBack）

    // --- マーク色 ×3（Mark1/2/3 Text/Back。候補B のマーク描画で使用） ---
    COLORREF markText[3] = { 0xFFFFFF, 0xFFFFFF, 0xFFFFFF };
    COLORREF markBack[3] = { 0x8000FF, 0x00FF00, 0xFF8080 };   // 紫 / 緑 / 水色

    // --- 比較結果色（CompareResult Text/Back） ---
    COLORREF compareText = 0xFFFFFF;
    COLORREF compareBack = 0x0000FF;   // 赤

    // --- 構造体編集の表示範囲色（原 設定+0x24 fg / +0x44 bg）。16進欄を青文字に ---
    COLORREF structText = 0xFF0000;   // 青（COLORREF 0x00BBGGRR）
    COLORREF structBack = 0xFFFFFF;   // 白（データ背景と同色）

    // --- レイアウト ---
    int lineSize    = 16;   // 1行バイト数（LineSize）
    int addressBase = 1;    // アドレス基数（AddressBase。0=10進 / 1=16進）

    // --- フォント（原 Height/Weight/Italic/CharSet/FaceName） ---
    //   lfHeight は負値=文字高さ(px)指定。原の見た目に合わせ -16（文字高さ16px）。
    int fontHeight = -16;
    int fontWeight = 400;         // 標準
    int fontItalic = 0;           // 非イタリック
    // フェイス名はワイドで保持（MBCS+/utf-8 のため。既定 ＭＳ ゴシック）。
    CStringW fontFace = L"ＭＳ ゴシック";   // "ＭＳ ゴシック"

    // --- 強調表示コード（原 設定+0x48 リスト）。コード値→強調文字色。順序保持・コードは一意 ---
    //   データビューの文字色（GetByteColor 既定枝）と、ビットイメージのパレット（反映ON時）が共用。
    std::vector<std::pair<BYTE, COLORREF>> hiCodes;

    // ビットイメージに指定色を反映させる（原 設定+0x5c）。ON: データ文字色＋強調コード＋
    //   データ背景色をビットイメージへ反映。OFF: 既定パレット（白/シアン/赤/黒）。
    bool bimgReflect = false;

    // バイト値別色表（256 COLORREF）を構築（原 FUN_00436aaa）。
    //   全バイト= dataText を埋め、各強調コードで table[code]=強調色 に上書きする。
    void BuildByteColorTable(COLORREF table[256]) const;

    // --- データ種別×8 の文字色/背景色アクセサ（色・フォントページ 107 用） ---
    //   索引順: 0=ヘッダー 1=アドレス 2=データ 3=マーク1 4=マーク2 5=マーク3 6=比較結果 7=構造体編集
    //   （原の登録値の並びと一致）。
    COLORREF& CategoryText(int i);
    COLORREF& CategoryBack(int i);
    static const int kCategoryCount = 8;

    // --- ドキュメント初期値の既定（新規/オープン時に doc へ反映する想定） ---
    int defCharset     = 1;  // 既定文字セット（CharcterSet。1=シフトJIS）
    int defByteOrderBig = 0; // 既定バイトオーダ（ByteOrder。0=リトル）

    // --- オープン時の既定（原 表示状態ページ 158 の 1015/1020/1025） ---
    bool openReadOnly   = false;  // ファイルオープン時に編集禁止とする
    bool openInsertMode = false;  // ファイルオープン時に挿入モードとする（false=上書き）
    bool openCharMode   = false;  // ファイルオープン時に文字入力モードとする（false=数値ペイン）
    // アドレスも横スクロールの対象とする（158 の 1011）。OFF時はアドレス欄を固定する。
    bool addrHScroll    = false;

    // --- レジストリ永続化（近代レイアウト。原の値名/バイナリ構造は再現しない） ---
    //   指定セクションへ全フィールドを往復（拡張子レコード毎に別セクション）。
    void Load(LPCTSTR section);
    void Save(LPCTSTR section) const;
};

// 拡張子別設定レコード（原 CMainFrame+0xba0 の各レコード相当）。
//   ext=拡張子パターン（既定は "*.*"＝すべてのファイル）、comment=コメント、s=表示設定一式。
struct CExtRecord {
    CStringW ext;
    CStringW comment;
    CStirlingSettings s;
};
