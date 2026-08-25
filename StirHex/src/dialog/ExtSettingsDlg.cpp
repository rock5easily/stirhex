// 拡張子別設定ダイアログ。一覧で複数レコードを管理し、各レコードの
// 表示状態／色・フォント設定を編集する。
#include "pch.h"
#include "app/UiStrings.h"   // ui::MsgBox（表題はアプリ名で統一）
#include "dialog/ExtSettingsDlg.h"
#include "app/StirlingApp.h"
#include "resource.h"
#include "util/ScopedGdi.h"   // GDI オブジェクトの RAII（Issue #48）

#include <afxdlgs.h>
#include <strsafe.h>   // StringCchCopyW

// キャラクターセットのラジオID（0..5 の順。1905 は欠番）。
static const UINT kCharsetIds[6] = {
    IDC_DISP_CS_ASCII, IDC_DISP_CS_SJIS, IDC_DISP_CS_EUC,
    IDC_DISP_CS_UNICODE, IDC_DISP_CS_EBCDIC, IDC_DISP_CS_EBCIDK,
};

// ===========================================================================
// CDisplayPage（子ダイアログ）
// ===========================================================================
BEGIN_MESSAGE_MAP(CDisplayPage, CDialog)
END_MESSAGE_MAP()

CDisplayPage::CDisplayPage(CWnd* pParent) : CDialog(IDD_SETTINGS_DISPLAY, pParent) {}

void CDisplayPage::ChargeFromSettings() {
    m_lineSize      = m_s.lineSize;
    m_addrRadix     = m_s.addressBase ? 1 : 0;
    m_byteOrderBig  = m_s.defByteOrderBig ? 1 : 0;
    m_addrHScroll   = m_s.addrHScroll ? TRUE : FALSE;
    m_openReadOnly  = m_s.openReadOnly ? TRUE : FALSE;
    m_openInsert    = m_s.openInsertMode ? TRUE : FALSE;
    m_openCharMode  = m_s.openCharMode ? TRUE : FALSE;
    m_charset       = (m_s.defCharset >= 0 && m_s.defCharset <= 5) ? m_s.defCharset : 1;
}

void CDisplayPage::HarvestToSettings() {
    m_s.lineSize        = m_lineSize;
    m_s.addressBase     = m_addrRadix ? 1 : 0;
    m_s.defByteOrderBig = m_byteOrderBig ? 1 : 0;
    m_s.addrHScroll     = (m_addrHScroll != FALSE);
    m_s.openReadOnly    = (m_openReadOnly != FALSE);
    m_s.openInsertMode  = (m_openInsert != FALSE);
    m_s.openCharMode    = (m_openCharMode != FALSE);
    m_s.defCharset      = m_charset;
}

void CDisplayPage::DoDataExchange(CDataExchange* pDX) {
    CDialog::DoDataExchange(pDX);
    DDX_Text(pDX, IDC_DISP_LINESIZE, m_lineSize);
    DDV_MinMaxInt(pDX, m_lineSize, 2, 256);
    DDX_Radio(pDX, IDC_DISP_RADIX_DEC, m_addrRadix);      // 1910=0 / 1911=1
    DDX_Radio(pDX, IDC_DISP_BO_LITTLE, m_byteOrderBig);   // 1920=0 / 1921=1
    DDX_Check(pDX, IDC_DISP_ADDR_HSCROLL, m_addrHScroll);
    DDX_Check(pDX, IDC_DISP_OPEN_READONLY, m_openReadOnly);
    DDX_Check(pDX, IDC_DISP_OPEN_INSERT, m_openInsert);
    DDX_Check(pDX, IDC_DISP_OPEN_CHARMODE, m_openCharMode);
    // キャラクターセットは非連続IDのため手動で往復する。
    if (pDX->m_bSaveAndValidate) {
        m_charset = 1;
        for (int i = 0; i < 6; ++i) {
            if (IsDlgButtonChecked(kCharsetIds[i])) { m_charset = i; break; }
        }
    } else {
        CheckRadioButton(kCharsetIds[0], kCharsetIds[5],
                         kCharsetIds[(m_charset >= 0 && m_charset <= 5) ? m_charset : 1]);
    }
}

BOOL CDisplayPage::OnInitDialog() {
    ChargeFromSettings();
    CDialog::OnInitDialog();   // DoDataExchange(FALSE) でコントロールへ反映

    // スピンを 1行バイト数エディットのバディに設定（2..256）。
    if (CSpinButtonCtrl* pSpin =
            (CSpinButtonCtrl*)GetDlgItem(IDC_DISP_LINESIZE_SPIN)) {
        pSpin->SetRange32(2, 256);
        pSpin->SetBuddy(GetDlgItem(IDC_DISP_LINESIZE));
        pSpin->SetPos(m_lineSize);
    }
    return TRUE;
}

// 子ダイアログの表示終了/OK時にホストから呼ぶ: コントロール→m_s へ回収。
void CDisplayPage::Harvest() {
    if (GetSafeHwnd() != nullptr && UpdateData(TRUE)) {
        HarvestToSettings();
    }
}

// ===========================================================================
// CColorFontPage（色・フォント）
// ===========================================================================
BEGIN_MESSAGE_MAP(CColorFontPage, CDialog)
    ON_CBN_SELCHANGE(IDC_CF_CATEGORY, &CColorFontPage::OnSelChangeCategory)
    ON_BN_CLICKED(IDC_CF_TEXTCOLOR, &CColorFontPage::OnTextColor)
    ON_BN_CLICKED(IDC_CF_BACKCOLOR, &CColorFontPage::OnBackColor)
    ON_BN_CLICKED(IDC_CF_FONT, &CColorFontPage::OnChooseFont)
    ON_BN_CLICKED(IDC_CF_RESET, &CColorFontPage::OnReset)
    ON_BN_CLICKED(IDC_CF_HL_ADD, &CColorFontPage::OnHlAdd)
    ON_BN_CLICKED(IDC_CF_HL_DEL, &CColorFontPage::OnHlDelete)
    ON_BN_CLICKED(IDC_CF_HL_CLEAR, &CColorFontPage::OnHlClear)
    ON_BN_CLICKED(IDC_CF_HL_EDIT, &CColorFontPage::OnHlEdit)
    ON_LBN_DBLCLK(IDC_CF_HILIST, &CColorFontPage::OnHlEdit)
    ON_BN_CLICKED(IDC_CF_BITIMAGE, &CColorFontPage::OnBitImageCheck)
    ON_WM_DRAWITEM()
    ON_WM_MEASUREITEM()
END_MESSAGE_MAP()

CColorFontPage::CColorFontPage(CWnd* pParent) : CDialog(IDD_COLOR_FONT, pParent) {}

void CColorFontPage::DoDataExchange(CDataExchange* pDX) {
    CDialog::DoDataExchange(pDX);
}

BOOL CColorFontPage::OnInitDialog() {
    CDialog::OnInitDialog();

    // データ種別コンボ（8カテゴリ。原の登録値の並び）。
    // 名称は文字列リソース（IDS_COLOR_HEADER から並び順に連番）。
    CStringW kNames[CStirlingSettings::kCategoryCount];
    for (int i = 0; i < CStirlingSettings::kCategoryCount; ++i) {
        kNames[i] = ui::LoadW(IDS_COLOR_HEADER + i);
    }
    if (CComboBox* pCombo = (CComboBox*)GetDlgItem(IDC_CF_CATEGORY)) {
        pCombo->ResetContent();
        for (int i = 0; i < CStirlingSettings::kCategoryCount; ++i) {
            pCombo->AddString(kNames[i]);
        }
        pCombo->SetCurSel(0);
    }
    m_category = 0;

    // 強調表示コード一覧＋ビットイメージ反映チェックを反映。
    RefillHiList();
    if (CButton* pChk = (CButton*)GetDlgItem(IDC_CF_BITIMAGE)) {
        pChk->SetCheck(m_s.bimgReflect ? BST_CHECKED : BST_UNCHECKED);
    }

    UpdateSwatches();
    return TRUE;
}

// 強調コード一覧の現在選択索引（無ければ -1）。
int CColorFontPage::HiCurSel() const {
    CListBox* pLb = (CListBox*)GetDlgItem(IDC_CF_HILIST);
    return (pLb != nullptr) ? pLb->GetCurSel() : LB_ERR;
}

// m_s.hiCodes を一覧(1021)へ反映（オーナードロー・HASSTRINGS無しのため索引=項目ID）。
void CColorFontPage::RefillHiList() {
    CListBox* pLb = (CListBox*)GetDlgItem(IDC_CF_HILIST);
    if (pLb == nullptr) { return; }
    const int prev = pLb->GetCurSel();
    pLb->ResetContent();
    for (size_t i = 0; i < m_s.hiCodes.size(); ++i) {
        pLb->AddString(_T(""));   // 描画は DrawHiItem（項目ID→hiCodes索引）
    }
    const int n = (int)m_s.hiCodes.size();
    if (n > 0) { pLb->SetCurSel((prev >= 0 && prev < n) ? prev : 0); }
}

void CColorFontPage::UpdateSwatches() {
    if (CWnd* w = GetDlgItem(IDC_CF_TEXTCOLOR)) { w->Invalidate(); }
    if (CWnd* w = GetDlgItem(IDC_CF_BACKCOLOR)) { w->Invalidate(); }
    if (CWnd* w = GetDlgItem(IDC_CF_PREVIEW))   { w->Invalidate(); }
}

void CColorFontPage::OnSelChangeCategory() {
    if (CComboBox* pCombo = (CComboBox*)GetDlgItem(IDC_CF_CATEGORY)) {
        const int sel = pCombo->GetCurSel();
        if (sel >= 0 && sel < CStirlingSettings::kCategoryCount) {
            m_category = sel;
            UpdateSwatches();
        }
    }
}

void CColorFontPage::OnTextColor() {
    CColorDialog dlg(m_s.CategoryText(m_category), CC_FULLOPEN | CC_RGBINIT, this);
    if (dlg.DoModal() == IDOK) {
        m_s.CategoryText(m_category) = dlg.GetColor();
        UpdateSwatches();
    }
}

void CColorFontPage::OnBackColor() {
    CColorDialog dlg(m_s.CategoryBack(m_category), CC_FULLOPEN | CC_RGBINIT, this);
    if (dlg.DoModal() == IDOK) {
        m_s.CategoryBack(m_category) = dlg.GetColor();
        UpdateSwatches();
    }
}

void CColorFontPage::OnChooseFont() {
    // 現在のフォント設定を LOGFONT へ（Unicode: LOGFONTW。フェイス名はワイドのまま扱う）。
    LOGFONTW lf = {0};
    lf.lfHeight = m_s.fontHeight;
    lf.lfWeight = m_s.fontWeight;
    lf.lfItalic = (BYTE)(m_s.fontItalic ? 1 : 0);
    lf.lfCharSet = SHIFTJIS_CHARSET;
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    ::StringCchCopyW(lf.lfFaceName, LF_FACESIZE, m_s.fontFace);

    CFontDialog dlg(&lf, CF_SCREENFONTS | CF_FIXEDPITCHONLY, nullptr, this);
    if (dlg.DoModal() == IDOK) {
        LOGFONTW out = {0};
        dlg.GetCurrentFont(&out);
        m_s.fontHeight = out.lfHeight;
        m_s.fontWeight = out.lfWeight;
        m_s.fontItalic = out.lfItalic ? 1 : 0;
        m_s.fontFace = out.lfFaceName;
        UpdateSwatches();   // プレビュー更新
    }
}

void CColorFontPage::OnReset() {
    // 原挙動: 確認ダイアログ（文字列1002「初期設定値に戻します」）で了承時のみ戻す。
    if (ui::MsgBoxRes(GetSafeHwnd(), IDS_EXT_RESET_CONFIRM, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
        return;
    }
    // 既定の色8ペア＋フォントへ戻す（表示状態フィールドは維持）。
    const CStirlingSettings def;
    for (int i = 0; i < CStirlingSettings::kCategoryCount; ++i) {
        m_s.CategoryText(i) = const_cast<CStirlingSettings&>(def).CategoryText(i);
        m_s.CategoryBack(i) = const_cast<CStirlingSettings&>(def).CategoryBack(i);
    }
    m_s.fontHeight = def.fontHeight;
    m_s.fontWeight = def.fontWeight;
    m_s.fontItalic = def.fontItalic;
    m_s.fontFace   = def.fontFace;
    // 強調表示コード・ビットイメージ反映も既定（空/OFF）へ。
    m_s.hiCodes.clear();
    m_s.bimgReflect = false;
    RefillHiList();
    if (CButton* pChk = (CButton*)GetDlgItem(IDC_CF_BITIMAGE)) {
        pChk->SetCheck(BST_UNCHECKED);
    }
    UpdateSwatches();
}

// ビットイメージに指定色を反映させる（原 設定+0x5c）。
void CColorFontPage::OnBitImageCheck() {
    if (CButton* pChk = (CButton*)GetDlgItem(IDC_CF_BITIMAGE)) {
        m_s.bimgReflect = (pChk->GetCheck() == BST_CHECKED);
    }
}

// 追加...: コード入力（187）→ 強調色選択。既存コードは色を更新（一意）。
void CColorFontPage::OnHlAdd() {
    CHiCodeDlg codeDlg(this, 0);
    if (codeDlg.DoModal() != IDOK) { return; }
    CColorDialog clrDlg(m_s.dataText, CC_FULLOPEN | CC_RGBINIT, this);
    if (clrDlg.DoModal() != IDOK) { return; }
    const COLORREF color = clrDlg.GetColor();
    // 既存の同一コードは色更新、なければ追加。
    bool found = false;
    for (auto& hc : m_s.hiCodes) {
        if (hc.first == codeDlg.m_code) { hc.second = color; found = true; break; }
    }
    if (!found) { m_s.hiCodes.emplace_back(codeDlg.m_code, color); }
    RefillHiList();
    // 追加/更新した項目を選択。
    if (CListBox* pLb = (CListBox*)GetDlgItem(IDC_CF_HILIST)) {
        for (size_t i = 0; i < m_s.hiCodes.size(); ++i) {
            if (m_s.hiCodes[i].first == codeDlg.m_code) { pLb->SetCurSel((int)i); break; }
        }
    }
}

// 削除: 選択の強調コードを確認の上で除去（原「選択された項目を削除します」）。
void CColorFontPage::OnHlDelete() {
    const int sel = HiCurSel();
    if (sel < 0 || sel >= (int)m_s.hiCodes.size()) { return; }
    if (ui::MsgBoxRes(GetSafeHwnd(), IDS_EXT_DELETE_CONFIRM, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
        return;
    }
    m_s.hiCodes.erase(m_s.hiCodes.begin() + sel);
    RefillHiList();
}

// 全削除: 確認の上で全消去（原「登録されている項目を全て削除します」）。
void CColorFontPage::OnHlClear() {
    if (m_s.hiCodes.empty()) { return; }
    if (ui::MsgBoxRes(GetSafeHwnd(), IDS_EXT_DELETE_ALL_CONFIRM, MB_OKCANCEL | MB_ICONQUESTION) != IDOK) {
        return;
    }
    m_s.hiCodes.clear();
    RefillHiList();
}

// 編集...: 選択項目の強調色を変更（コードは変更しない。原ヘルプの挙動）。
void CColorFontPage::OnHlEdit() {
    const int sel = HiCurSel();
    if (sel < 0 || sel >= (int)m_s.hiCodes.size()) { return; }
    CColorDialog clrDlg(m_s.hiCodes[sel].second, CC_FULLOPEN | CC_RGBINIT, this);
    if (clrDlg.DoModal() != IDOK) { return; }
    m_s.hiCodes[sel].second = clrDlg.GetColor();
    if (CListBox* pLb = (CListBox*)GetDlgItem(IDC_CF_HILIST)) {
        pLb->Invalidate();
    }
}

void CColorFontPage::DrawSwatch(LPDRAWITEMSTRUCT dis, COLORREF color) {
    CDC dc; dc.Attach(dis->hDC);
    CRect rc = dis->rcItem;
    dc.FillSolidRect(rc, color);
    dc.DrawEdge(rc, (dis->itemState & ODS_SELECTED) ? EDGE_SUNKEN : EDGE_RAISED, BF_RECT);
    dc.Detach();
}

// (行,列)→適用カテゴリ色＋カテゴリ種別（原 FUN_0040850f のデモパターン）。
//   既定=データ色。行0: 01=マーク1/02=マーク2/03=マーク3。行2: 22-2B=構造体。
//   行3: 37-3F=比較。行4: 40-43=比較。戻り値: 0=データ 6=比較 7=構造体（帯化対象）。
int CColorFontPage::PreviewCellColor(int row, int col, COLORREF& fg, COLORREF& bg) const {
    fg = m_s.dataText; bg = m_s.dataBack;
    if (row == 0 && col == 1) { fg = m_s.markText[0]; bg = m_s.markBack[0]; return 3; }
    if (row == 0 && col == 2) { fg = m_s.markText[1]; bg = m_s.markBack[1]; return 4; }
    if (row == 0 && col == 3) { fg = m_s.markText[2]; bg = m_s.markBack[2]; return 5; }
    if (row == 2 && col >= 2 && col <= 11) { fg = m_s.structText; bg = m_s.structBack; return 7; }   // 22-2B
    if (row == 3 && col >= 7 && col <= 15) { fg = m_s.compareText; bg = m_s.compareBack; return 6; }  // 37-3F
    if (row == 4 && col >= 0 && col <= 3)  { fg = m_s.compareText; bg = m_s.compareBack; return 6; }  // 40-43
    return 0;
}

void CColorFontPage::DrawPreview(LPDRAWITEMSTRUCT dis) {
    CDC dc; dc.Attach(dis->hDC);
    CRect rc = dis->rcItem;
    dc.FillSolidRect(rc, m_s.dataBack);
    dc.DrawEdge(rc, EDGE_SUNKEN, BF_RECT);

    // 設定フォントで描画（原はプレビュー用に設定フォントを使用）。
    LOGFONTW lf = {0};
    lf.lfHeight = m_s.fontHeight;
    lf.lfWeight = m_s.fontWeight;
    lf.lfItalic = (BYTE)(m_s.fontItalic ? 1 : 0);
    lf.lfCharSet = SHIFTJIS_CHARSET;
    lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
    wcscpy_s(lf.lfFaceName, LF_FACESIZE,
             m_s.fontFace.IsEmpty() ? L"ＭＳ ゴシック" : (LPCWSTR)m_s.fontFace);
    stirling::ScopedGdiObject font(::CreateFontIndirectW(&lf));
    stirling::ScopedSelectHdc selFont(dis->hDC, font.Get());
    ::SetBkMode(dis->hDC, OPAQUE);

    int cw = 8, ch = 16;
    { TEXTMETRICW tm; if (::GetTextMetricsW(dis->hDC, &tm)) { cw = tm.tmAveCharWidth; ch = tm.tmHeight; } }
    if (ch <= 0) { ch = 16; }

    const int x0 = rc.left + 2;
    const int addrCols = 10;                 // " XXXXXXXX " 相当（10桁）
    const int hexX = x0 + addrCols * cw;      // 16進欄開始X
    int y = rc.top + 2;

    // ヘッダ行: ADDRESS ＋ 00 01 ... 0F（ヘッダ色）。行全幅をヘッダ背景で塗る。
    dc.FillSolidRect(rc.left, y, rc.Width(), ch, m_s.headerBack);
    dc.SetTextColor(m_s.headerText); dc.SetBkColor(m_s.headerBack);
    dc.TextOutW(x0 + cw, y, L"ADDRESS");
    for (int c = 0; c < 16; ++c) {
        CStringW h; h.Format(L"%02X", c);
        dc.TextOutW(hexX + c * 3 * cw, y, h);
    }
    y += ch;

    // データ 6 行（00-5F）。アドレス欄＝アドレス色、16進＝バイト毎にデモ色。
    const int rows = 6;
    for (int r = 0; r < rows; ++r) {
        if (y + ch > rc.bottom) { break; }
        // アドレス欄背景＋アドレス
        dc.FillSolidRect(rc.left, y, hexX - rc.left, ch, m_s.addrBack);
        CStringW addr; addr.Format(L" %08X ", r * 16);
        dc.SetTextColor(m_s.addrText); dc.SetBkColor(m_s.addrBack);
        dc.TextOutW(x0, y, addr);
        // 16進バイト
        for (int c = 0; c < 16; ++c) {
            COLORREF fg, bg;
            const int cat = PreviewCellColor(r, c, fg, bg);
            CStringW cell; cell.Format(L"%02X", r * 16 + c);
            dc.SetTextColor(fg); dc.SetBkColor(bg);
            dc.TextOutW(hexX + c * 3 * cw, y, cell);
            // 比較(6)/構造体(7)は隣接同種の桁間スペースも背景色で帯化（原挙動）。
            if ((cat == 6 || cat == 7) && c + 1 < 16) {
                COLORREF nfg, nbg;
                if (PreviewCellColor(r, c + 1, nfg, nbg) == cat) {
                    dc.FillSolidRect(hexX + c * 3 * cw + 2 * cw, y, cw, ch, bg);
                }
            }
        }
        y += ch;
    }

    dc.Detach();
}

// 強調コード一覧の1項目（原準拠）: 左にコード番号("XX" テキスト)、右に強調色のスウォッチ矩形。
//   背景/文字色は選択状態に従う（通常=リスト色、選択=ハイライト色）。
void CColorFontPage::DrawHiItem(LPDRAWITEMSTRUCT dis) {
    const int idx = (int)dis->itemID;
    if (idx < 0 || idx >= (int)m_s.hiCodes.size()) { return; }
    CDC dc; dc.Attach(dis->hDC);
    CRect rc = dis->rcItem;
    const bool selected = (dis->itemState & ODS_SELECTED) != 0;
    const COLORREF back = selected ? ::GetSysColor(COLOR_HIGHLIGHT) : ::GetSysColor(COLOR_WINDOW);
    const COLORREF text = selected ? ::GetSysColor(COLOR_HIGHLIGHTTEXT) : ::GetSysColor(COLOR_WINDOWTEXT);
    dc.FillSolidRect(rc, back);

    // コード番号（16進2桁、原は接頭辞なし）。
    dc.SetTextColor(text);
    dc.SetBkMode(TRANSPARENT);
    CStringW s; s.Format(L"%02X", m_s.hiCodes[idx].first);
    CRect textRc = rc; textRc.left += 6;
    dc.DrawTextW(s, &textRc, DT_SINGLELINE | DT_VCENTER | DT_LEFT);

    // 強調色スウォッチ（右側。項目高に応じた枠付き矩形）。
    CRect sw = rc;
    sw.left = rc.left + 34;
    sw.right = rc.right - 4;
    sw.DeflateRect(0, 2);
    if (sw.right > sw.left) {
        dc.FillSolidRect(sw, m_s.hiCodes[idx].second);
        dc.Draw3dRect(sw, RGB(128, 128, 128), RGB(128, 128, 128));
    }
    dc.Detach();
}

void CColorFontPage::OnDrawItem(int nIDCtl, LPDRAWITEMSTRUCT lpDIS) {
    if (nIDCtl == IDC_CF_TEXTCOLOR) { DrawSwatch(lpDIS, m_s.CategoryText(m_category)); return; }
    if (nIDCtl == IDC_CF_BACKCOLOR) { DrawSwatch(lpDIS, m_s.CategoryBack(m_category)); return; }
    if (nIDCtl == IDC_CF_PREVIEW)   { DrawPreview(lpDIS); return; }
    if (nIDCtl == IDC_CF_HILIST)    { DrawHiItem(lpDIS); return; }
    CDialog::OnDrawItem(nIDCtl, lpDIS);
}

void CColorFontPage::OnMeasureItem(int nIDCtl, LPMEASUREITEMSTRUCT lpMIS) {
    if (nIDCtl == IDC_CF_HILIST) { lpMIS->itemHeight = 16; return; }
    CDialog::OnMeasureItem(nIDCtl, lpMIS);
}

// ===========================================================================
// CExtRecordDlg（拡張子/コメント ヘッダ ＋ タブ切替の子ダイアログ2枚）
// ===========================================================================
BEGIN_MESSAGE_MAP(CExtRecordDlg, CDialog)
    ON_NOTIFY(TCN_SELCHANGE, IDC_EXTREC_SHEET, &CExtRecordDlg::OnTabChange)
END_MESSAGE_MAP()

CExtRecordDlg::CExtRecordDlg(const CExtRecord& rec, bool isDefault, CWnd* pParent)
    : CDialog(IDD_EXT_RECORD, pParent), m_rec(rec), m_isDefault(isDefault) {}

BOOL CExtRecordDlg::OnInitDialog() {
    CDialog::OnInitDialog();

    // 上部ヘッダ: 拡張子/コメント。既定レコードは編集不可。
    if (CWnd* w = GetDlgItem(IDC_EXTREC_EXT))     { w->SetWindowText(m_rec.ext); }
    if (CWnd* w = GetDlgItem(IDC_EXTREC_COMMENT)) { w->SetWindowText(m_rec.comment); }
    if (m_isDefault) {
        if (CWnd* w = GetDlgItem(IDC_EXTREC_EXT))     { w->EnableWindow(FALSE); }
        if (CWnd* w = GetDlgItem(IDC_EXTREC_COMMENT)) { w->EnableWindow(FALSE); }
    }

    CTabCtrl* pTab = (CTabCtrl*)GetDlgItem(IDC_EXTREC_SHEET);
    if (pTab == nullptr) { return TRUE; }
    pTab->InsertItem(0, ui::LoadW(IDS_EXT_TAB_DISPLAY));
    pTab->InsertItem(1, ui::LoadW(IDS_EXT_TAB_COLORFONT));

    // 子ダイアログ2枚を自然サイズで生成し、大きい方に合わせてタブ/ホストを動的に整える。
    m_display.m_s   = m_rec.s;
    m_colorFont.m_s = m_rec.s;
    m_display.Create(IDD_SETTINGS_DISPLAY, this);
    m_colorFont.Create(IDD_COLOR_FONT, this);
    CRect pr1, pr2;
    m_display.GetWindowRect(&pr1);
    m_colorFont.GetWindowRect(&pr2);
    const int pageW = max(pr1.Width(), pr2.Width());
    const int pageH = max(pr1.Height(), pr2.Height());

    // タブは左上位置を保ち、表示領域がページを収めるようウィンドウサイズを算出。
    CRect tabRc; pTab->GetWindowRect(&tabRc); ScreenToClient(&tabRc);
    CRect need(0, 0, pageW, pageH);
    pTab->AdjustRect(TRUE, &need);            // 表示領域→タブ窓サイズ
    const int tabW = need.Width();
    const int tabH = need.Height();
    pTab->SetWindowPos(nullptr, tabRc.left, tabRc.top, tabW, tabH, SWP_NOZORDER | SWP_NOACTIVATE);

    // 子ダイアログをタブ表示領域の左上へ（自然サイズのまま）。
    CRect disp(tabRc.left, tabRc.top, tabRc.left + tabW, tabRc.top + tabH);
    pTab->AdjustRect(FALSE, &disp);
    m_display.SetWindowPos(&wndTop, disp.left, disp.top, pageW, pageH, SWP_NOACTIVATE);
    m_colorFont.SetWindowPos(&wndTop, disp.left, disp.top, pageW, pageH, SWP_NOACTIVATE);
    ShowTab(0);

    // OK/キャンセルをタブの下へ中央寄せ、ホストのサイズをフィットさせる。
    const int leftMargin = tabRc.left;
    const int cliW = tabRc.left + tabW + leftMargin;
    const int btnTop = tabRc.top + tabH + 8;
    CRect okRc; GetDlgItem(IDOK)->GetWindowRect(&okRc); ScreenToClient(&okRc);
    const int bw = okRc.Width(), bh = okRc.Height(), gap = 8;
    const int bx = (cliW - (bw * 2 + gap)) / 2;
    GetDlgItem(IDOK)->SetWindowPos(nullptr, bx, btnTop, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    GetDlgItem(IDCANCEL)->SetWindowPos(nullptr, bx + bw + gap, btnTop, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    const int cliH = btnTop + bh + 8;

    CRect wr, cr; GetWindowRect(&wr); GetClientRect(&cr);
    SetWindowPos(nullptr, 0, 0, cliW + (wr.Width() - cr.Width()),
                 cliH + (wr.Height() - cr.Height()), SWP_NOMOVE | SWP_NOZORDER);
    CenterWindow();
    return TRUE;
}

void CExtRecordDlg::ShowTab(int idx) {
    m_display.ShowWindow(idx == 0 ? SW_SHOW : SW_HIDE);
    m_colorFont.ShowWindow(idx == 1 ? SW_SHOW : SW_HIDE);
}

void CExtRecordDlg::OnTabChange(NMHDR* /*pNMHDR*/, LRESULT* pResult) {
    CTabCtrl* pTab = (CTabCtrl*)GetDlgItem(IDC_EXTREC_SHEET);
    if (pTab != nullptr) { ShowTab(pTab->GetCurSel()); }
    if (pResult) { *pResult = 0; }
}

void CExtRecordDlg::OnOK() {
    // ヘッダの拡張子/コメントを回収（既定レコードは元の値を維持）。
    if (!m_isDefault) {
        if (CWnd* w = GetDlgItem(IDC_EXTREC_EXT))     { w->GetWindowText(m_rec.ext); }
        if (CWnd* w = GetDlgItem(IDC_EXTREC_COMMENT)) { w->GetWindowText(m_rec.comment); }
    }
    // 各ページを回収して設定へマージ（表示状態→色フォントの色/フォント）。
    m_display.Harvest();
    CStirlingSettings s = m_display.m_s;
    CStirlingSettings c = m_colorFont.m_s;
    for (int i = 0; i < CStirlingSettings::kCategoryCount; ++i) {
        s.CategoryText(i) = c.CategoryText(i);
        s.CategoryBack(i) = c.CategoryBack(i);
    }
    s.fontHeight = c.fontHeight;
    s.fontWeight = c.fontWeight;
    s.fontItalic = c.fontItalic;
    s.fontFace   = c.fontFace;
    s.hiCodes    = c.hiCodes;       // 強調表示コード
    s.bimgReflect = c.bimgReflect;  // ビットイメージ反映
    m_rec.s = s;
    CDialog::OnOK();
}

// ===========================================================================
// CHiCodeDlg（強調表示コード入力 IDD_HIGHLIGHT_CODE 187）
// ===========================================================================
BEGIN_MESSAGE_MAP(CHiCodeDlg, CDialog)
END_MESSAGE_MAP()

CHiCodeDlg::CHiCodeDlg(CWnd* pParent, BYTE init)
    : CDialog(IDD_HIGHLIGHT_CODE, pParent), m_code(init) {}

BOOL CHiCodeDlg::OnInitDialog() {
    CDialog::OnInitDialog();
    // 入力欄は空欄で開始（原挙動）。
    if (CWnd* w = GetDlgItem(IDC_HL_CODE)) { w->SetWindowText(_T("")); }
    return TRUE;
}

void CHiCodeDlg::OnOK() {
    CString s;
    if (CWnd* w = GetDlgItem(IDC_HL_CODE)) { w->GetWindowText(s); }
    s.Trim();
    if (s.GetLength() > 2 && (s.Left(2) == _T("0x") || s.Left(2) == _T("0X"))) {
        s = s.Mid(2);
    }
    // 16進1バイト（0..FF）として検証。
    if (s.IsEmpty() || s.GetLength() > 2) {
        ui::MsgBoxRes(GetSafeHwnd(), IDS_HEX_BYTE_INPUT);
        return;
    }
    int val = 0;
    for (int i = 0; i < s.GetLength(); ++i) {
        const wchar_t ch = s[i];
        int d;
        if (ch >= L'0' && ch <= L'9')      { d = ch - L'0'; }
        else if (ch >= L'A' && ch <= L'F') { d = ch - L'A' + 10; }
        else if (ch >= L'a' && ch <= L'f') { d = ch - L'a' + 10; }
        else {
            ui::MsgBoxRes(GetSafeHwnd(), IDS_HEX_BYTE_INPUT);
            return;
        }
        val = val * 16 + d;
    }
    m_code = (BYTE)(val & 0xFF);
    CDialog::OnOK();
}

// ===========================================================================
// CExtListDlg（拡張子別設定一覧）
// ===========================================================================
BEGIN_MESSAGE_MAP(CExtListDlg, CDialog)
    ON_BN_CLICKED(IDC_EXTLIST_SETTINGS, &CExtListDlg::OnSettings)
    ON_BN_CLICKED(IDC_EXTLIST_ADD, &CExtListDlg::OnAdd)
    ON_BN_CLICKED(IDC_EXTLIST_DELETE, &CExtListDlg::OnDelete)
    ON_LBN_DBLCLK(IDC_EXTLIST_LIST, &CExtListDlg::OnDblClk)
END_MESSAGE_MAP()

CExtListDlg::CExtListDlg(CWnd* pParent) : CDialog(IDD_EXT_LIST, pParent) {
    m_records = theApp.ExtRecords();   // 作業コピー
}

BOOL CExtListDlg::OnInitDialog() {
    CDialog::OnInitDialog();
    RefillList();
    return TRUE;
}

void CExtListDlg::RefillList() {
    CListBox* pList = (CListBox*)GetDlgItem(IDC_EXTLIST_LIST);
    if (pList == nullptr) { return; }
    const int keep = pList->GetCurSel();
    pList->ResetContent();
    for (const CExtRecord& rec : m_records) {
        // "(*.<ext>)comment" 形式（原の "(*.*)すべてのファイル" に倣う。ext は "*." を除いた部分）。
        CStringW line = L"(*." + rec.ext + L")" + rec.comment;
        pList->AddString(line);
    }
    if (keep >= 0 && keep < (int)m_records.size()) { pList->SetCurSel(keep); }
    else if (!m_records.empty()) { pList->SetCurSel(0); }
}

int CExtListDlg::CurSel() const {
    CListBox* pList = (CListBox*)GetDlgItem(IDC_EXTLIST_LIST);
    if (pList == nullptr) { return -1; }
    const int sel = pList->GetCurSel();
    return (sel >= 0 && sel < (int)m_records.size()) ? sel : -1;
}

void CExtListDlg::OnSettings() {
    const int sel = CurSel();
    if (sel < 0) { ::MessageBeep(0); return; }
    CExtRecordDlg dlg(m_records[sel], /*isDefault=*/sel == 0, this);
    if (dlg.DoModal() == IDOK) {
        m_records[sel] = dlg.m_rec;
        RefillList();
    }
}

void CExtListDlg::OnAdd() {
    CExtRecord rec;   // 既定設定・空の拡張子/コメント
    CExtRecordDlg dlg(rec, /*isDefault=*/false, this);
    if (dlg.DoModal() == IDOK) {
        m_records.push_back(dlg.m_rec);
        RefillList();
        CListBox* pList = (CListBox*)GetDlgItem(IDC_EXTLIST_LIST);
        if (pList != nullptr) { pList->SetCurSel((int)m_records.size() - 1); }
    }
}

void CExtListDlg::OnDelete() {
    const int sel = CurSel();
    if (sel == 0) {   // 索引0=基本設定 "*" は削除不可（原: 文字列1052）。
        ui::MsgBoxRes(GetSafeHwnd(), IDS_EXT_BASE_UNDELETABLE);
        return;
    }
    if (sel < 0) { ::MessageBeep(0); return; }
    m_records.erase(m_records.begin() + sel);
    RefillList();
}

void CExtListDlg::OnDblClk() { OnSettings(); }
