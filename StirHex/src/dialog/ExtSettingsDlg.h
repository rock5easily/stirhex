// 拡張子別設定ダイアログ（原 0x8051 → 一覧185→レコード編集→表示状態158＋色フォント107）。
//   レコード編集はホストダイアログ(253)＋タブコントロール＋子ダイアログ2枚で構成する
//   （原の上部ヘッダ＋タブUXに準拠。プロパティシート埋め込みはフォーカス再取得で不安定なため不使用）。
#pragma once

#include "app/StirlingSettings.h"

#include <vector>

// 「表示状態」ページ（子ダイアログ IDD_SETTINGS_DISPLAY 158）。
//   1行バイト数・アドレス基数・キャラクターセット・バイトオーダー・オープン時既定を編集。
class CDisplayPage : public CDialog {
public:
    CDisplayPage(CWnd* pParent = nullptr);

    CStirlingSettings m_s;   // 作業コピー（OnInitDialog で流し込み、Harvest で回収）
    void Harvest();          // コントロール→m_s へ回収（生成済みの時）

protected:
    // DDX 用中間値
    int  m_lineSize = 16;
    int  m_addrRadix = 1;     // 0=10進 / 1=16進（DDX_Radio: 1910/1911）
    int  m_byteOrderBig = 0;  // 0=リトル / 1=ビッグ（DDX_Radio: 1920/1921）
    BOOL m_addrHScroll = FALSE;
    BOOL m_openReadOnly = FALSE;
    BOOL m_openInsert = FALSE;
    BOOL m_openCharMode = FALSE;
    int  m_charset = 1;       // 0..5（非連続IDのため手動マッピング）

    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();

    void ChargeFromSettings();   // m_s → 中間値/コントロール反映用
    void HarvestToSettings();    // 中間値 → m_s
    DECLARE_MESSAGE_MAP()
};

// 強調表示コード入力ダイアログ（IDD_HIGHLIGHT_CODE 187）。16進1バイトのコードを入力する。
class CHiCodeDlg : public CDialog {
public:
    CHiCodeDlg(CWnd* pParent, BYTE init = 0);
    BYTE m_code;   // in/out（0..0xFF）

protected:
    virtual BOOL OnInitDialog();
    virtual void OnOK();
    DECLARE_MESSAGE_MAP()
};

// 「色・フォント」ページ（子ダイアログ IDD_COLOR_FONT 107）。データ種別×8 の文字色/背景色、
//   表示フォント、強調表示コード（コード→強調文字色）、ビットイメージ反映を編集する。
class CColorFontPage : public CDialog {
public:
    CColorFontPage(CWnd* pParent = nullptr);

    CStirlingSettings m_s;   // 作業コピー（色/フォントのみ編集。表示状態フィールドは src のまま）
    void Harvest() {}        // 色/フォントは即時 m_s へ反映済（回収不要。対称性のため用意）

protected:
    int m_category = 0;      // 現在選択中のデータ種別（0..7）

    virtual void DoDataExchange(CDataExchange* pDX);
    virtual BOOL OnInitDialog();

    void UpdateSwatches();   // 選択カテゴリの色をスウォッチ/プレビューへ反映
    void DrawSwatch(LPDRAWITEMSTRUCT dis, COLORREF color);
    void DrawPreview(LPDRAWITEMSTRUCT dis);
    void DrawHiItem(LPDRAWITEMSTRUCT dis);   // 強調コード一覧の1項目（"0xXX" ＋ 強調色）
    int  PreviewCellColor(int row, int col, COLORREF& fg, COLORREF& bg) const;  // デモ色＋カテゴリ種別

    void RefillHiList();     // m_s.hiCodes を一覧(1021)へ反映
    int  HiCurSel() const;   // 一覧の現在選択索引（無ければ -1）

    afx_msg void OnSelChangeCategory();
    afx_msg void OnTextColor();
    afx_msg void OnBackColor();
    afx_msg void OnChooseFont();
    afx_msg void OnReset();
    afx_msg void OnHlAdd();       // 追加...（コード入力→色選択）
    afx_msg void OnHlDelete();    // 削除
    afx_msg void OnHlClear();     // 全削除
    afx_msg void OnHlEdit();      // 編集...（選択の強調色を変更）
    afx_msg void OnBitImageCheck();  // ビットイメージに指定色を反映させる
    afx_msg void OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDIS);
    afx_msg void OnMeasureItem(int nIDCtl, LPMEASUREITEMSTRUCT lpMIS);
    DECLARE_MESSAGE_MAP()
};

// 拡張子別設定 レコード編集ダイアログ（IDD_EXT_RECORD 253）。上部ヘッダに拡張子/コメント欄、
//   その下にタブ(1500)で表示状態/色フォントの子ダイアログを切替える（原のUXに準拠）。
class CExtRecordDlg : public CDialog {
public:
    CExtRecordDlg(const CExtRecord& rec, bool isDefault, CWnd* pParent = nullptr);

    CExtRecord m_rec;         // in/out（OKで確定）
    bool       m_isDefault;   // 既定 "*" レコードは拡張子/コメント編集不可

protected:
    CDisplayPage   m_display;
    CColorFontPage m_colorFont;

    virtual BOOL OnInitDialog();
    virtual void OnOK();
    void ShowTab(int idx);    // idx=0:表示状態 / 1:色フォント
    afx_msg void OnTabChange(NMHDR* pNMHDR, LRESULT* pResult);
    DECLARE_MESSAGE_MAP()
};

// 拡張子別設定一覧ダイアログ（IDD_EXT_LIST 185）。拡張子レコードの追加/削除/編集。
//   閉じる(OK)で theApp のレコードへ反映＋保存＋全ビュー再読込。
class CExtListDlg : public CDialog {
public:
    CExtListDlg(CWnd* pParent = nullptr);

    std::vector<CExtRecord> m_records;   // 作業コピー（OK で theApp へ反映）

protected:
    virtual BOOL OnInitDialog();
    void RefillList();                    // m_records をリストへ反映
    int  CurSel() const;                  // 現在選択レコード索引（無ければ -1）

    afx_msg void OnSettings();            // 設定...（選択を編集）
    afx_msg void OnAdd();                 // 追加...
    afx_msg void OnDelete();              // 削除
    afx_msg void OnDblClk();              // ダブルクリック＝設定
    DECLARE_MESSAGE_MAP()
};
