// バイナリパッチの作成／適用ダイアログ（Issue #259）。
#include "pch.h"

#include "dialog/BinaryPatchDlg.h"

#include "app/StirlingApp.h"
#include "app/ShellUtil.h"
#include "app/UiStrings.h"
#include "doc/StirlingDoc.h"

#include <algorithm>
#include <climits>
#include <cwchar>
#include <cstring>
#include <new>
#include <system_error>
#include <utility>

namespace {

std::wstring ControlText(const CWnd* owner, UINT id) {
    if (owner == nullptr) { return {}; }
    CWnd* control = owner->GetDlgItem(id);
    if (control == nullptr) { return {}; }
    CStringW text;
    control->GetWindowText(text);
    return std::wstring(text.GetString(), static_cast<size_t>(text.GetLength()));
}

bool FileSize(const std::wstring& path, stirling::FileOffset& size) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
        (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }
    ULARGE_INTEGER value = {};
    value.HighPart = data.nFileSizeHigh;
    value.LowPart = data.nFileSizeLow;
    if (value.QuadPart > static_cast<ULONGLONG>(LLONG_MAX)) { return false; }
    size = static_cast<stirling::FileOffset>(value.QuadPart);
    return true;
}

std::wstring FullPath(const std::wstring& path) {
    if (path.empty()) { return {}; }
    DWORD needed = ::GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (needed == 0) { return path; }
    std::wstring buffer(static_cast<size_t>(needed), L'\0');
    DWORD length = ::GetFullPathNameW(path.c_str(), needed, buffer.data(), nullptr);
    if (length == 0 || length >= needed) { return path; }
    buffer.resize(length);
    return buffer;
}

struct FileIdentity {
    DWORD volume = 0;
    DWORD indexHigh = 0;
    DWORD indexLow = 0;
};

bool ReadIdentity(const std::wstring& path, FileIdentity& out) {
    HANDLE handle = ::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS,
                                   nullptr);
    if (handle == INVALID_HANDLE_VALUE) { return false; }
    BY_HANDLE_FILE_INFORMATION info = {};
    const BOOL ok = ::GetFileInformationByHandle(handle, &info);
    ::CloseHandle(handle);
    if (!ok) { return false; }
    out.volume = info.dwVolumeSerialNumber;
    out.indexHigh = info.nFileIndexHigh;
    out.indexLow = info.nFileIndexLow;
    return true;
}

bool SameFile(const std::wstring& left, const std::wstring& right) {
    if (left.empty() || right.empty()) { return false; }
    if (_wcsicmp(FullPath(left).c_str(), FullPath(right).c_str()) == 0) { return true; }
    FileIdentity a = {}, b = {};
    if (!ReadIdentity(left, a) || !ReadIdentity(right, b)) { return false; }
    return a.volume == b.volume && a.indexHigh == b.indexHigh && a.indexLow == b.indexLow;
}

bool IsIpsPatch(const std::wstring& path) {
    HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { return false; }
    char magic[5] = {};
    DWORD got = 0;
    const BOOL ok = ::ReadFile(handle, magic, sizeof(magic), &got, nullptr);
    ::CloseHandle(handle);
    return ok && got == 5 && ::memcmp(magic, "PATCH", 5) == 0;
}

bool IsBpsPatch(const std::wstring& path) {
    HANDLE handle = ::CreateFileW(path.c_str(), GENERIC_READ,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { return false; }
    char magic[4] = {};
    DWORD got = 0;
    const BOOL ok = ::ReadFile(handle, magic, sizeof(magic), &got, nullptr);
    ::CloseHandle(handle);
    return ok && got == 4 && ::memcmp(magic, "BPS1", 4) == 0;
}

}  // namespace

BEGIN_MESSAGE_MAP(CBinaryPatchDlg, CDialog)
    ON_BN_CLICKED(IDC_BP_MODE_CREATE, &CBinaryPatchDlg::OnModeChanged)
    ON_BN_CLICKED(IDC_BP_MODE_APPLY, &CBinaryPatchDlg::OnModeChanged)
    ON_BN_CLICKED(IDC_BP_FORMAT_BPS, &CBinaryPatchDlg::OnFormatChanged)
    ON_BN_CLICKED(IDC_BP_FORMAT_IPS, &CBinaryPatchDlg::OnFormatChanged)
    ON_BN_CLICKED(IDC_BP_SOURCE_BROWSE, &CBinaryPatchDlg::OnBrowseSource)
    ON_BN_CLICKED(IDC_BP_SECOND_BROWSE, &CBinaryPatchDlg::OnBrowseSecond)
    ON_BN_CLICKED(IDC_BP_RESULT_BROWSE, &CBinaryPatchDlg::OnBrowseResult)
    ON_BN_CLICKED(IDC_BP_START, &CBinaryPatchDlg::OnStart)
    ON_BN_CLICKED(IDC_BP_STOP, &CBinaryPatchDlg::OnStop)
    ON_EN_CHANGE(IDC_BP_SOURCE_EDIT, &CBinaryPatchDlg::OnPathChanged)
    ON_EN_CHANGE(IDC_BP_SECOND_EDIT, &CBinaryPatchDlg::OnPathChanged)
    ON_EN_CHANGE(IDC_BP_RESULT_EDIT, &CBinaryPatchDlg::OnPathChanged)
    ON_WM_TIMER()
    ON_WM_DESTROY()
END_MESSAGE_MAP()

CBinaryPatchDlg::CBinaryPatchDlg(Mode mode, CWnd* parent)
    : CDialog(IDD_BINARY_PATCH, parent), mode_(mode) {}

CBinaryPatchDlg::~CBinaryPatchDlg() {
    JoinWorker();
}

BOOL CBinaryPatchDlg::OnInitDialog() {
    CDialog::OnInitDialog();
    CheckRadioButton(IDC_BP_MODE_CREATE, IDC_BP_MODE_APPLY,
                     mode_ == Mode::Generate ? IDC_BP_MODE_CREATE : IDC_BP_MODE_APPLY);
    CheckRadioButton(IDC_BP_FORMAT_BPS, IDC_BP_FORMAT_IPS, IDC_BP_FORMAT_BPS);
    SetDlgItemText(IDC_BP_STATUS, ui::LoadW(IDS_BP_READY));
    UpdateModeUi();
    return TRUE;
}

void CBinaryPatchDlg::OnOK() {
    OnStart();
}

void CBinaryPatchDlg::OnCancel() {
    if (running_) {
        closing_ = true;
        if (workerState_ != nullptr) { workerState_->cancel.store(true, std::memory_order_release); }
        JoinWorker();
        running_ = false;
        KillTimer(kProgressTimer);
    }
    CDialog::OnCancel();
}

void CBinaryPatchDlg::OnDestroy() {
    if (running_ && workerState_ != nullptr) {
        workerState_->cancel.store(true, std::memory_order_release);
    }
    KillTimer(kProgressTimer);
    JoinWorker();
    running_ = false;
    CDialog::OnDestroy();
}

void CBinaryPatchDlg::OnModeChanged() {
    if (running_) { return; }
    mode_ = IsDlgButtonChecked(IDC_BP_MODE_APPLY) == BST_CHECKED
                ? Mode::Apply : Mode::Generate;
    SetDlgItemText(IDC_BP_STATUS, ui::LoadW(IDS_BP_READY));
    UpdateModeUi();
}

void CBinaryPatchDlg::OnFormatChanged() {
    if (running_) { return; }
    ipsSelected_ = IsDlgButtonChecked(IDC_BP_FORMAT_IPS) == BST_CHECKED;
    UpdateIpsAvailability();
}

void CBinaryPatchDlg::OnPathChanged() {
    if (!running_) {
        UpdateIpsAvailability();
        UpdateStartAvailability();
    }
}

void CBinaryPatchDlg::UpdateModeUi() {
    const bool generate = mode_ == Mode::Generate;
    SetDlgItemText(IDC_BP_SOURCE_LABEL, ui::LoadW(IDS_BP_SOURCE));
    SetDlgItemText(IDC_BP_SECOND_LABEL,
                   ui::LoadW(generate ? IDS_BP_TARGET : IDS_BP_PATCH));
    SetDlgItemText(IDC_BP_RESULT_LABEL,
                   ui::LoadW(generate ? IDS_BP_PATCH : IDS_BP_RESULT));
    SetDlgItemText(IDC_BP_START,
                   ui::LoadW(generate ? IDS_BP_START_CREATE : IDS_BP_START_APPLY));
    SetDlgItemText(IDC_BP_FORMAT_LABEL, ui::LoadW(IDS_BP_FORMAT));

    const UINT formatIds[] = {IDC_BP_FORMAT_BPS, IDC_BP_FORMAT_IPS};
    for (UINT id : formatIds) {
        if (CWnd* w = GetDlgItem(id)) { w->EnableWindow(generate ? TRUE : FALSE); }
    }
    if (CWnd* open = GetDlgItem(IDC_BP_OPEN_RESULT)) {
        open->ShowWindow(generate ? SW_HIDE : SW_SHOW);
        open->EnableWindow(generate ? FALSE : TRUE);
    }
    UpdateIpsAvailability();
    UpdateStartAvailability();
}

void CBinaryPatchDlg::UpdateStartAvailability() {
    CWnd* start = GetDlgItem(IDC_BP_START);
    if (start == nullptr) { return; }
    const bool hasRequiredPaths = !ControlText(this, IDC_BP_SOURCE_EDIT).empty() &&
                                  !ControlText(this, IDC_BP_SECOND_EDIT).empty() &&
                                  !ControlText(this, IDC_BP_RESULT_EDIT).empty();
    start->EnableWindow(!running_ && hasRequiredPaths ? TRUE : FALSE);
}

bool CBinaryPatchDlg::IpsAllowed(const std::wstring& source,
                                 const std::wstring& target) const {
    stirling::FileOffset sourceSize = 0;
    stirling::FileOffset targetSize = 0;
    if (!FileSize(source, sourceSize) || !FileSize(target, targetSize)) { return false; }
    return sourceSize == targetSize &&
           sourceSize <= stirling::BinaryPatchLimits::kIpsGenerateMaxBytes &&
           targetSize <= stirling::BinaryPatchLimits::kIpsGenerateMaxBytes;
}

void CBinaryPatchDlg::UpdateIpsAvailability() {
    CWnd* ips = GetDlgItem(IDC_BP_FORMAT_IPS);
    if (ips == nullptr) { return; }
    if (mode_ != Mode::Generate) {
        ips->EnableWindow(FALSE);
        const std::wstring patch = ControlText(this, IDC_BP_SECOND_EDIT);
        if (IsIpsPatch(patch)) {
            SetDlgItemText(IDC_BP_FORMAT_NOTE, ui::LoadW(IDS_BP_IPS_UNVERIFIED));
        } else if (IsBpsPatch(patch)) {
            SetDlgItemText(IDC_BP_FORMAT_NOTE, ui::LoadW(IDS_BP_BPS_VERIFIED));
        } else {
            SetDlgItemText(IDC_BP_FORMAT_NOTE, ui::LoadW(IDS_BP_AUTO));
        }
        return;
    }

    const std::wstring source = ControlText(this, IDC_BP_SOURCE_EDIT);
    const std::wstring target = ControlText(this, IDC_BP_SECOND_EDIT);
    SetDlgItemText(IDC_BP_FORMAT_NOTE, CStringW());
    const bool allowed = IpsAllowed(source, target);
    ips->EnableWindow(allowed ? TRUE : FALSE);
    if (!allowed && ipsSelected_) {
        ipsSelected_ = false;
        CheckRadioButton(IDC_BP_FORMAT_BPS, IDC_BP_FORMAT_IPS, IDC_BP_FORMAT_BPS);
    }
    if (!running_ && !source.empty() && !target.empty() && !allowed) {
        SetDlgItemText(IDC_BP_STATUS, ui::LoadW(IDS_BP_IPS_CONDITION));
    }
}

void CBinaryPatchDlg::BrowseInput(UINT editId, UINT filterId) {
    CStringW filter = ui::LoadW(filterId);
    CFileDialog dialog(TRUE, nullptr, nullptr,
                       OFN_HIDEREADONLY | OFN_FILEMUSTEXIST | OFN_EXPLORER,
                       filter, this);
    if (dialog.DoModal() == IDOK) {
        SetDlgItemText(editId, dialog.GetPathName());
        UpdateIpsAvailability();
    }
}

void CBinaryPatchDlg::BrowseOutput() {
    const bool generate = mode_ == Mode::Generate;
    const bool ips = IsDlgButtonChecked(IDC_BP_FORMAT_IPS) == BST_CHECKED;
    const UINT filterId = generate ? (ips ? IDS_BP_FILTER_IPS : IDS_BP_FILTER_BPS)
                                   : IDS_BP_FILTER_INPUT;
    const wchar_t* extension = generate ? (ips ? L"ips" : L"bps") : nullptr;
    CStringW filter = ui::LoadW(filterId);
    CFileDialog dialog(FALSE, extension, nullptr,
                       OFN_EXPLORER | OFN_PATHMUSTEXIST,
                       filter, this);
    if (dialog.DoModal() == IDOK) {
        SetDlgItemText(IDC_BP_RESULT_EDIT, dialog.GetPathName());
        UpdateIpsAvailability();
    }
}

void CBinaryPatchDlg::OnBrowseSource() {
    BrowseInput(IDC_BP_SOURCE_EDIT, IDS_BP_FILTER_INPUT);
}

void CBinaryPatchDlg::OnBrowseSecond() {
    BrowseInput(IDC_BP_SECOND_EDIT,
                mode_ == Mode::Generate ? IDS_BP_FILTER_INPUT : IDS_BP_FILTER_PATCH);
}

void CBinaryPatchDlg::OnBrowseResult() {
    BrowseOutput();
}

bool CBinaryPatchDlg::ReadPaths(std::wstring& first, std::wstring& second,
                                std::wstring& result) const {
    first = ControlText(this, IDC_BP_SOURCE_EDIT);
    second = ControlText(this, IDC_BP_SECOND_EDIT);
    result = ControlText(this, IDC_BP_RESULT_EDIT);
    return !first.empty() && !second.empty() && !result.empty();
}

bool CBinaryPatchDlg::OutputConflicts(const std::wstring& output,
                                      const std::wstring& first,
                                      const std::wstring& second) const {
    if (SameFile(output, first) || SameFile(output, second)) { return true; }

    POSITION templatePos = theApp.GetFirstDocTemplatePosition();
    while (templatePos != nullptr) {
        CDocTemplate* docTemplate = theApp.GetNextDocTemplate(templatePos);
        if (docTemplate == nullptr) { continue; }
        POSITION docPos = docTemplate->GetFirstDocPosition();
        while (docPos != nullptr) {
            CDocument* document = docTemplate->GetNextDoc(docPos);
            CStirlingDoc* stirlingDoc = DYNAMIC_DOWNCAST(CStirlingDoc, document);
            if (stirlingDoc == nullptr) { continue; }
            const CString path = stirlingDoc->GetPathName();
            if (!path.IsEmpty() && SameFile(output, static_cast<LPCWSTR>(path))) {
                return true;
            }
        }
    }
    return false;
}

bool CBinaryPatchDlg::ValidatePaths(const std::wstring& first,
                                    const std::wstring& second,
                                    const std::wstring& result) const {
    if (first.empty() || second.empty()) {
        ui::MsgBoxRes(m_hWnd, IDS_BP_PATH_REQUIRED);
        return false;
    }
    if (result.empty()) {
        ui::MsgBoxRes(m_hWnd, IDS_BP_OUTPUT_REQUIRED);
        return false;
    }
    stirling::FileOffset ignored = 0;
    if (!FileSize(first, ignored) || !FileSize(second, ignored)) {
        ui::MsgBoxRes(m_hWnd, IDS_BP_INPUT_NOT_FOUND);
        return false;
    }
    WIN32_FILE_ATTRIBUTE_DATA outputAttributes = {};
    if (::GetFileAttributesExW(result.c_str(), GetFileExInfoStandard, &outputAttributes) &&
        (outputAttributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        ui::MsgBoxRes(m_hWnd, IDS_BP_OUTPUT_REQUIRED);
        return false;
    }
    if (mode_ == Mode::Generate &&
        IsDlgButtonChecked(IDC_BP_FORMAT_IPS) == BST_CHECKED &&
        !IpsAllowed(first, second)) {
        ui::MsgBoxRes(m_hWnd, IDS_BP_IPS_NOT_ALLOWED);
        return false;
    }
    if (OutputConflicts(result, first, second)) {
        // The output field is labelled "patch file" when creating (Issue #272).
        CStringW message;
        message.Format(ui::LoadW(mode_ == Mode::Generate ? IDS_BP_OUTPUT_CONFLICT_PATCH
                                                         : IDS_BP_OUTPUT_CONFLICT),
                       result.c_str());
        ui::MsgBox(m_hWnd, message);
        return false;
    }
    return true;
}

void CBinaryPatchDlg::SetRunning(bool running) {
    running_ = running;
    const UINT edits[] = {IDC_BP_MODE_CREATE, IDC_BP_MODE_APPLY,
                          IDC_BP_SOURCE_EDIT, IDC_BP_SOURCE_BROWSE,
                          IDC_BP_SECOND_EDIT, IDC_BP_SECOND_BROWSE,
                          IDC_BP_RESULT_EDIT, IDC_BP_RESULT_BROWSE,
                          IDC_BP_FORMAT_BPS, IDC_BP_FORMAT_IPS, IDC_BP_OPEN_RESULT,
                          IDC_BP_START};
    for (UINT id : edits) {
        if (CWnd* w = GetDlgItem(id)) { w->EnableWindow(running ? FALSE : TRUE); }
    }
    if (CWnd* stop = GetDlgItem(IDC_BP_STOP)) { stop->EnableWindow(running ? TRUE : FALSE); }
    if (!running) { UpdateModeUi(); }
}

void CBinaryPatchDlg::StartWorker(const std::wstring& first,
                                  const std::wstring& second,
                                  const std::wstring& result) {
    try {
        workerState_ = std::make_shared<WorkerState>();
    } catch (const std::bad_alloc&) {
        ui::MsgBoxRes(m_hWnd, IDS_BP_STATUS_MEMORY, MB_OK | MB_ICONERROR);
        return;
    }
    outputPath_ = result;
    const bool generate = mode_ == Mode::Generate;
    const stirling::BinaryPatchFormat format =
        IsDlgButtonChecked(IDC_BP_FORMAT_IPS) == BST_CHECKED
            ? stirling::BinaryPatchFormat::kIps
            : stirling::BinaryPatchFormat::kBps;
    const std::shared_ptr<WorkerState> state = workerState_;

    try {
        worker_ = std::make_unique<std::thread>(
            [state, generate, format, first, second, result]() {
                stirling::BinaryPatchOptions options;
                options.progress = [state](stirling::FileOffset processed,
                                           stirling::FileOffset total) {
                    state->processed.store(processed, std::memory_order_relaxed);
                    state->total.store(total, std::memory_order_relaxed);
                    return !state->cancel.load(std::memory_order_acquire);
                };
                stirling::BinaryPatchResult operation;
                try {
                    operation = generate
                        ? stirling::GenerateBinaryPatch(first.c_str(), second.c_str(),
                                                        result.c_str(), format, options)
                        : stirling::ApplyBinaryPatch(first.c_str(), second.c_str(),
                                                     result.c_str(), options);
                } catch (const std::bad_alloc&) {
                    operation.status = stirling::BinaryPatchStatus::kOutOfMemory;
                } catch (const std::system_error& error) {
                    operation.status = stirling::BinaryPatchStatus::kOpenFailed;
                    operation.systemError = static_cast<unsigned long>(error.code().value());
                } catch (const std::exception&) {
                    operation.status = stirling::BinaryPatchStatus::kUnsupported;
                    operation.systemError = ERROR_GEN_FAILURE;
                }
                state->result = std::move(operation);
                state->done.store(true, std::memory_order_release);
            });
    } catch (const std::bad_alloc&) {
        workerState_.reset();
        ui::MsgBoxRes(m_hWnd, IDS_BP_STATUS_MEMORY, MB_OK | MB_ICONERROR);
        return;
    } catch (const std::system_error& error) {
        workerState_.reset();
        ui::MsgBox(m_hWnd,
                   ui::AppendErrorReason(ui::LoadW(IDS_BP_STATUS_OPEN_FAILED),
                                         static_cast<DWORD>(error.code().value())),
                   MB_OK | MB_ICONERROR);
        return;
    } catch (const std::exception&) {
        workerState_.reset();
        ui::MsgBoxRes(m_hWnd, IDS_BP_STATUS_UNSUPPORTED, MB_OK | MB_ICONERROR);
        return;
    }
    SetRunning(true);
    CStringW progressText;
    progressText.Format(ui::LoadW(IDS_BP_PROGRESS), 0LL, 0LL);
    SetDlgItemText(IDC_BP_STATUS, progressText);
    CProgressCtrl* progress = static_cast<CProgressCtrl*>(GetDlgItem(IDC_BP_PROGRESS));
    if (progress != nullptr) {
        progress->SetRange32(0, 100);
        progress->SetPos(0);
    }
    if (!SetTimer(kProgressTimer, 50, nullptr)) {
        if (workerState_ != nullptr) {
            workerState_->cancel.store(true, std::memory_order_release);
        }
        JoinWorker();
        FinishWorker();
    }
}

void CBinaryPatchDlg::OnStart() {
    if (running_) { return; }
    std::wstring first, second, result;
    if (!ReadPaths(first, second, result) || !ValidatePaths(first, second, result)) { return; }
    WIN32_FILE_ATTRIBUTE_DATA outputAttributes = {};
    if (::GetFileAttributesExW(result.c_str(), GetFileExInfoStandard, &outputAttributes) &&
        (outputAttributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        CStringW message;
        message.Format(ui::LoadW(IDS_BP_OVERWRITE), result.c_str());
        if (ui::MsgBox(m_hWnd, message, MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return;
        }
    }
    StartWorker(first, second, result);
}

void CBinaryPatchDlg::OnStop() {
    if (!running_ || workerState_ == nullptr) { return; }
    workerState_->cancel.store(true, std::memory_order_release);
}

void CBinaryPatchDlg::OnTimer(UINT_PTR timer) {
    if (timer != kProgressTimer) {
        CDialog::OnTimer(timer);
        return;
    }
    if (workerState_ == nullptr) { return; }
    const stirling::FileOffset processed = workerState_->processed.load(std::memory_order_relaxed);
    const stirling::FileOffset total = workerState_->total.load(std::memory_order_relaxed);
    CProgressCtrl* progress = static_cast<CProgressCtrl*>(GetDlgItem(IDC_BP_PROGRESS));
    if (progress != nullptr && total > 0) {
        const stirling::FileOffset percent = (processed >= total)
            ? 100 : (processed * 100 / total);
        progress->SetPos(static_cast<int>(std::clamp<stirling::FileOffset>(percent, 0, 100)));
    }
    if (total > 0) {
        CStringW text;
        text.Format(ui::LoadW(IDS_BP_PROGRESS),
                    static_cast<long long>(processed), static_cast<long long>(total));
        SetDlgItemText(IDC_BP_STATUS, text);
    }
    if (workerState_->done.load(std::memory_order_acquire)) {
        FinishWorker();
    }
}

void CBinaryPatchDlg::JoinWorker() {
    if (worker_ != nullptr && worker_->joinable()) { worker_->join(); }
    worker_.reset();
}

void CBinaryPatchDlg::FinishWorker() {
    KillTimer(kProgressTimer);
    const std::shared_ptr<WorkerState> state = workerState_;
    JoinWorker();
    workerState_.reset();
    SetRunning(false);
    if (state == nullptr) { return; }
    const stirling::BinaryPatchResult result = state->result;
    if (result.Ok()) {
        PresentResult(result);
    } else {
        PresentFailure(result);
    }
}

void CBinaryPatchDlg::PresentResult(const stirling::BinaryPatchResult& /*result*/) {
    const UINT textId = mode_ == Mode::Generate ? IDS_BP_CREATE_DONE : IDS_BP_APPLY_DONE;
    CStringW message;
    message.Format(ui::LoadW(textId), outputPath_.c_str());
    SetDlgItemText(IDC_BP_STATUS, message);
    if (mode_ != Mode::Apply || closing_ ||
        IsDlgButtonChecked(IDC_BP_OPEN_RESULT) != BST_CHECKED) {
        return;
    }

    CDocument* document = theApp.OpenDroppedFile(outputPath_.c_str());
    if (document == nullptr) {
        CStringW error;
        error.Format(ui::LoadW(IDS_BP_RESULT_OPEN_FAILED), outputPath_.c_str());
        ui::MsgBox(m_hWnd, error, MB_OK | MB_ICONEXCLAMATION);
        return;
    }
    resultOpened_ = true;
    EndDialog(IDOK);
}

void CBinaryPatchDlg::PresentFailure(const stirling::BinaryPatchResult& result) {
    if (result.status == stirling::BinaryPatchStatus::kCancelled) {
        SetDlgItemText(IDC_BP_STATUS, ui::LoadW(IDS_BP_CANCELLED));
        return;
    }
    CStringW reason = ui::LoadW(StatusStringId(result.status));
    if (result.systemError != ERROR_SUCCESS) {
        reason += L"\n";
        reason += ui::FormatSystemError(result.systemError);
    }
    reason += ui::KeptTempPathNoteW(result.keptTempPath);
    CStringW message;
    message.Format(ui::LoadW(IDS_BP_OPERATION_FAILED), reason.GetString());
    SetDlgItemText(IDC_BP_STATUS, reason);
    ui::MsgBox(m_hWnd, message, MB_OK | MB_ICONEXCLAMATION);
}

UINT CBinaryPatchDlg::StatusStringId(stirling::BinaryPatchStatus status) {
    switch (status) {
    case stirling::BinaryPatchStatus::kInvalidArgument: return IDS_BP_STATUS_INVALID_ARG;
    case stirling::BinaryPatchStatus::kOpenFailed:      return IDS_BP_STATUS_OPEN_FAILED;
    case stirling::BinaryPatchStatus::kReadFailed:      return IDS_BP_STATUS_READ_FAILED;
    case stirling::BinaryPatchStatus::kWriteFailed:     return IDS_BP_STATUS_WRITE_FAILED;
    case stirling::BinaryPatchStatus::kInvalidPatch:    return IDS_BP_STATUS_INVALID_PATCH;
    case stirling::BinaryPatchStatus::kSourceMismatch:  return IDS_BP_STATUS_SOURCE_MISMATCH;
    case stirling::BinaryPatchStatus::kTargetMismatch:  return IDS_BP_STATUS_TARGET_MISMATCH;
    case stirling::BinaryPatchStatus::kCancelled:       return IDS_BP_STATUS_CANCELLED;
    case stirling::BinaryPatchStatus::kLimitExceeded:   return IDS_BP_STATUS_LIMIT;
    case stirling::BinaryPatchStatus::kOutOfMemory:     return IDS_BP_STATUS_MEMORY;
    case stirling::BinaryPatchStatus::kConflict:        return IDS_BP_STATUS_CONFLICT;
    case stirling::BinaryPatchStatus::kUnsupported:     return IDS_BP_STATUS_UNSUPPORTED;
    case stirling::BinaryPatchStatus::kOk:              return IDS_BP_READY;
    default:                                            return IDS_BP_STATUS_UNKNOWN;
    }
}
