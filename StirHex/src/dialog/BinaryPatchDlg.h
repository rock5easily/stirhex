// バイナリパッチの作成／適用ダイアログ（Issue #259）。
#pragma once

#include "core/BinaryPatch.h"
#include "resource.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>

class CBinaryPatchDlg : public CDialog {
public:
    enum class Mode { Generate, Apply };

    explicit CBinaryPatchDlg(Mode mode, CWnd* parent = nullptr);
    ~CBinaryPatchDlg() override;

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    void OnCancel() override;

    afx_msg void OnModeChanged();
    afx_msg void OnFormatChanged();
    afx_msg void OnBrowseSource();
    afx_msg void OnBrowseSecond();
    afx_msg void OnBrowseResult();
    afx_msg void OnStart();
    afx_msg void OnStop();
    afx_msg void OnTimer(UINT_PTR timer);
    afx_msg void OnDestroy();
    afx_msg void OnPathChanged();
    DECLARE_MESSAGE_MAP()

private:
    struct WorkerState {
        std::atomic<bool> cancel{false};
        std::atomic<bool> done{false};
        std::atomic<stirling::FileOffset> processed{0};
        std::atomic<stirling::FileOffset> total{0};
        stirling::BinaryPatchResult result;
    };

    static constexpr UINT_PTR kProgressTimer = 1;

    void UpdateModeUi();
    void UpdateStartAvailability();
    void UpdateIpsAvailability();
    void BrowseInput(UINT editId, UINT filterId);
    void BrowseOutput();
    bool ReadPaths(std::wstring& first, std::wstring& second,
                   std::wstring& result) const;
    bool ValidatePaths(const std::wstring& first, const std::wstring& second,
                       const std::wstring& result) const;
    bool IpsAllowed(const std::wstring& source, const std::wstring& target) const;
    bool OutputConflicts(const std::wstring& output,
                         const std::wstring& first,
                         const std::wstring& second) const;
    void StartWorker(const std::wstring& first, const std::wstring& second,
                     const std::wstring& result);
    void FinishWorker();
    void JoinWorker();
    void SetRunning(bool running);
    void PresentResult(const stirling::BinaryPatchResult& result);
    void PresentFailure(const stirling::BinaryPatchResult& result);
    static UINT StatusStringId(stirling::BinaryPatchStatus status);

    Mode mode_;
    bool running_ = false;
    bool closing_ = false;
    bool resultOpened_ = false;
    bool ipsSelected_ = false;
    std::wstring outputPath_;
    std::shared_ptr<WorkerState> workerState_;
    std::unique_ptr<std::thread> worker_;
};
