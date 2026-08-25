// CSelectRangeDlg 実装（原 IDD_SELECT_RANGE=202, FUN_00405de0 系）。
#include "pch.h"
#include "dialog/SelectRangeDlg.h"

BEGIN_MESSAGE_MAP(CSelectRangeDlg, CDialog)
END_MESSAGE_MAP()

CSelectRangeDlg::CSelectRangeDlg(CWnd* pParent, stirling::FileOffset total, bool hasSel,
                                 stirling::FileOffset selStart, stirling::FileOffset selEnd)
    : CDialog(IDD_SELECT_RANGE, pParent)
    , m_total(total)
    , m_hasSel(hasSel)
    , m_selStart(selStart)
    , m_selEnd(selEnd)
    , m_start(0)
    , m_end(total > 0 ? total - 1 : 0)
{
    // 原 ctor（FUN_00405de0）: 範囲バーを this+0x5c に生成し、total/hasSel/selStart/selEnd を保持。
}

// 原 OnInitDialog: 範囲バー（IDD 200）を子として生成し、STATIC プレースホルダ(1130)の位置へ配置。
BOOL CSelectRangeDlg::OnInitDialog() {
    CDialog::OnInitDialog();

    m_rangeBar.SetRange(m_total, m_hasSel, m_selStart, m_selEnd);
    if (m_rangeBar.Create(IDD_PRINT_RANGE_BAR, this)) {
        // 原テンプレートのプレースホルダ STATIC(1130) 左上へ配置（無ければ 7,7 相当へフォールバック）。
        CRect anchor;
        if (CWnd* pAnchor = GetDlgItem(IDC_SELRANGE_ANCHOR)) {
            pAnchor->GetWindowRect(&anchor);
            ScreenToClient(&anchor);
        } else {
            CRect fallback(7, 7, 15, 15);
            MapDialogRect(&fallback);
            anchor = fallback;
        }
        m_rangeBar.SetWindowPos(nullptr, anchor.left, anchor.top, 0, 0,
                                SWP_NOSIZE | SWP_NOZORDER);
        m_rangeBar.ShowWindow(SW_SHOW);
        m_rangeBar.SetPaneEnabled(TRUE);   // 常に有効（選択範囲チェックは選択ありのときのみ活性）
    }
    return TRUE;
}

// 原 OnOK: 範囲バーを検証し、成功時のみ範囲を確定して閉じる。
void CSelectRangeDlg::OnOK() {
    stirling::FileOffset s = 0, e = 0;
    if (!m_rangeBar.Validate(s, e)) { return; }   // 不正なら閉じない（原と同じ）
    m_start = s;
    m_end   = e;
    CDialog::OnOK();
}
