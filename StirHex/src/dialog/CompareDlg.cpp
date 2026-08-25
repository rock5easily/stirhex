// CCompareDlg 実装（原 IDD_COMPARE=166, FUN_00409600/FUN_00409686 系）。
#include "pch.h"
#include "dialog/CompareDlg.h"
#include "doc/StirlingDoc.h"
#include "view/StirlingView.h"

BEGIN_MESSAGE_MAP(CCompareDlg, CDialog)
END_MESSAGE_MAP()

CCompareDlg::CCompareDlg(CWnd* pParent, CStirlingDoc* selfDoc)
    : CDialog(IDD_COMPARE, pParent)
    , m_selfDoc(selfDoc)
    , m_targetView(nullptr)
{
}

void CCompareDlg::DoDataExchange(CDataExchange* pDX) {
    CDialog::DoDataExchange(pDX);
}

BOOL CCompareDlg::OnInitDialog() {
    CDialog::OnInitDialog();

    // 現在の文書以外の開いている文書の各ビューを、フレームタイトルで一覧（原 FUN_00409686）。
    //   ItemData に対象ビュー（CStirlingView*）を格納する。
    CListBox* pList = static_cast<CListBox*>(GetDlgItem(IDC_COMPARE_LIST));
    if (pList == nullptr) { return TRUE; }
    pList->ResetContent();

    int count = 0;
    CWinApp* pApp = AfxGetApp();
    POSITION posT = pApp->GetFirstDocTemplatePosition();
    while (posT != nullptr) {
        CDocTemplate* pTmpl = pApp->GetNextDocTemplate(posT);
        POSITION posD = pTmpl->GetFirstDocPosition();
        while (posD != nullptr) {
            CDocument* pDoc = pTmpl->GetNextDoc(posD);
            if (pDoc == static_cast<CDocument*>(m_selfDoc)) { continue; }   // 自分は除外
            POSITION posV = pDoc->GetFirstViewPosition();
            while (posV != nullptr) {
                CView* pView = pDoc->GetNextView(posV);
                CStirlingView* pSV = DYNAMIC_DOWNCAST(CStirlingView, pView);
                if (pSV == nullptr) { continue; }
                CString title;
                if (CFrameWnd* pFrame = pSV->GetParentFrame()) {
                    pFrame->GetWindowText(title);   // フレームタイトル（原はビューの親フレーム）
                }
                if (title.IsEmpty()) { title = pDoc->GetTitle(); }
                const int idx = pList->AddString(title);
                pList->SetItemData(idx, reinterpret_cast<DWORD_PTR>(pSV));
                ++count;
            }
        }
    }
    if (count > 0) { pList->SetCurSel(0); }   // 先頭を選択（原 FUN_00407ec0）
    return TRUE;
}

void CCompareDlg::OnOK() {
    CListBox* pList = static_cast<CListBox*>(GetDlgItem(IDC_COMPARE_LIST));
    if (pList != nullptr) {
        const int sel = pList->GetCurSel();
        if (sel != LB_ERR) {
            m_targetView = reinterpret_cast<CStirlingView*>(pList->GetItemData(sel));
        }
    }
    if (m_targetView == nullptr) {
        ::MessageBeep(0);   // 未選択は確定しない
        return;
    }
    CDialog::OnOK();
}
