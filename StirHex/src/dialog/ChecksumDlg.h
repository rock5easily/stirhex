// CChecksumDlg — 計算条件と結果を表示するモーダルダイアログ（Issue #226）。
// 表示中は対象文書の寿命と内容を維持し、終了時に計算を中止する。
#pragma once
#include "core/Checksum.h"
#include "resource.h"

class CStirlingDoc;
class CChecksumDlg : public CDialog {
public:
    CChecksumDlg(CWnd* parent, CStirlingDoc& doc, bool selected,
                 stirling::FileOffset start, stirling::FileOffset end);
protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    void OnCancel() override;
    afx_msg void OnConditionsChanged();
    afx_msg void OnCopy();
    afx_msg void OnStop();
    afx_msg void OnTimer(UINT_PTR timer);
    afx_msg void OnDestroy();
    DECLARE_MESSAGE_MAP()
private:
    void UpdateRange();
    void UpdateRunning(bool running);
    void PresentResult();
    CStirlingDoc& doc_;
    bool selected_;
    stirling::FileOffset selectionStart_, selectionEnd_, start_ = 0, length_ = 0;
    long changeSeq_ = 0;
    stirling::Checksum checksum_;
    static constexpr UINT_PTR kTimer = 1;
};
