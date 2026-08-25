// CPrintRangeDlg 実装（原 IDD_PRINT_RANGE=201, FUN_0042cee0 系）。
#include "pch.h"
#include "dialog/PrintRangeDlg.h"

BEGIN_MESSAGE_MAP(CPrintRangeDlg, CDialog)
END_MESSAGE_MAP()

CPrintRangeDlg::CPrintRangeDlg(CWnd* pParent, stirling::FileOffset total, bool hasSel,
                               stirling::FileOffset selStart, stirling::FileOffset selEnd)
    : CDialog(IDD_PRINT_RANGE, pParent)
    , m_total(total)
    , m_hasSel(hasSel)
    , m_selStart(selStart)
    , m_selEnd(selEnd)
    , m_start(0)
    , m_end(total > 0 ? total - 1 : 0)
    , m_preview(0)   // 原 ctor: this+0x5c=0（既定は印刷）
{
}

void CPrintRangeDlg::DoDataExchange(CDataExchange* pDX) {
    CDialog::DoDataExchange(pDX);
    // 原 DoDataExchange（FUN_0042cf7e）: DDX_Check(pDX, 1011, this+0x5c)。
    DDX_Check(pDX, IDC_PRINTRANGE_PREVIEW, m_preview);
}

// 原 OnInitDialog（FUN_0042cfbc）: base + 範囲バー生成（FUN_0042d004）+ SetPaneEnabled(TRUE)。
BOOL CPrintRangeDlg::OnInitDialog() {
    CDialog::OnInitDialog();

    // 範囲入力バー（IDD 200）を子として生成し、チェックボックスの下へ配置（原 FUN_0042d004）。
    m_rangeBar.SetRange(m_total, m_hasSel, m_selStart, m_selEnd);
    if (m_rangeBar.Create(IDD_PRINT_RANGE_BAR, this)) {
        // チェックボックス（1011）の左端・その 8 単位下へ（原はコントロール矩形から算出）。
        CRect cbRect;
        if (CWnd* pCb = GetDlgItem(IDC_PRINTRANGE_PREVIEW)) {
            pCb->GetWindowRect(&cbRect);
            ScreenToClient(&cbRect);
        } else {
            CRect fallback(7, 7, 97, 17);
            MapDialogRect(&fallback);
            cbRect = fallback;
        }
        m_rangeBar.SetWindowPos(nullptr, cbRect.left, cbRect.bottom + 8, 0, 0,
                                SWP_NOSIZE | SWP_NOZORDER);
        m_rangeBar.ShowWindow(SW_SHOW);
        m_rangeBar.SetPaneEnabled(TRUE);   // 印刷範囲ダイアログでは常に有効（原 FUN_00430cfc(,1)）
    }
    return TRUE;
}

// 原 OnOK（FUN_0042d0d2）: 範囲バーを検証し、成功時のみ範囲を確定して閉じる。
void CPrintRangeDlg::OnOK() {
    UpdateData(TRUE);   // m_preview（チェック状態）

    stirling::FileOffset s = 0, e = 0;
    if (!m_rangeBar.Validate(s, e)) { return; }   // 不正なら閉じない（原と同じ）
    m_start = s;
    m_end   = e;
    CDialog::OnOK();
}
