// CReplaceConfirmDlg 実装。
#include "pch.h"
#include "dialog/ReplaceConfirmDlg.h"

BEGIN_MESSAGE_MAP(CReplaceConfirmDlg, CDialog)
    ON_BN_CLICKED(IDC_RCONF_EXEC, &CReplaceConfirmDlg::OnExec)
    ON_BN_CLICKED(IDC_RCONF_SKIP, &CReplaceConfirmDlg::OnSkip)
    ON_BN_CLICKED(IDC_RCONF_ALL,  &CReplaceConfirmDlg::OnAll)
END_MESSAGE_MAP()
