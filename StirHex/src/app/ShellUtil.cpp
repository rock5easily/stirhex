// ShellUtil 実装。IFileDialog / ShellExecuteEx によるシェル操作のラッパ。
#include "pch.h"
#include "app/ShellUtil.h"
#include "app/UiStrings.h"   // ui::LoadW（失敗理由の書式はリソースから）
#include "resource.h"

#include <atlbase.h>    // CComPtr（COM ポインタの RAII）
#include <shlobj.h>     // SHCreateItemFromParsingName, IFileDialog,
                        //   SHParseDisplayName, SHOpenFolderAndSelectItems
#include <shellapi.h>   // ShellExecuteExW

#include "util/PathParts.h"   // 開くフォルダの決定（Issue #133）

namespace ui {

namespace {

// CoTaskMemAlloc されたワイド文字列の RAII（GetDisplayName の戻り値用）。
class CoTaskMemString {
public:
    CoTaskMemString() = default;
    ~CoTaskMemString() { ::CoTaskMemFree(m_p); }
    CoTaskMemString(const CoTaskMemString&) = delete;
    CoTaskMemString& operator=(const CoTaskMemString&) = delete;

    PWSTR* Receive() { return &m_p; }
    PCWSTR Get() const { return m_p; }

private:
    PWSTR m_p = nullptr;
};

// 初期フォルダを設定する（指定が無い／解決できない場合は何もしない＝シェル既定に委ねる）。
void ApplyInitialFolder(IFileDialog* dialog, LPCWSTR initialFolder) {
    if (initialFolder == nullptr) { return; }

    CStringW path(initialFolder);
    path.Trim(L" \t\"");   // 貼り付けでクォートが付いたパスも初期位置として扱う
    if (path.IsEmpty()) { return; }

    CComPtr<IShellItem> item;
    // 存在しないパスはここで失敗する。初期位置が決まらないだけなので致命ではない。
    if (FAILED(::SHCreateItemFromParsingName(path, nullptr, IID_PPV_ARGS(&item)))) { return; }
    dialog->SetFolder(item);
}

}  // namespace

HRESULT BrowseForFolder(HWND owner, LPCWSTR title, LPCWSTR initialFolder, CStringW& outPath) {
    CComPtr<IFileDialog> dialog;
    HRESULT hr = dialog.CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER);
    if (FAILED(hr)) { return hr; }

    DWORD options = 0;
    hr = dialog->GetOptions(&options);
    if (FAILED(hr)) { return hr; }
    // FORCEFILESYSTEM: 仮想フォルダ（コントロールパネル等）を選ばせない＝必ずパスが取れる。
    hr = dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                            FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
    if (FAILED(hr)) { return hr; }

    if (title != nullptr && *title != L'\0') { dialog->SetTitle(title); }
    ApplyInitialFolder(dialog, initialFolder);

    // キャンセルは HRESULT_FROM_WIN32(ERROR_CANCELLED)。呼び出し側が IsUserCancelled で区別する。
    hr = dialog->Show(owner);
    if (FAILED(hr)) { return hr; }

    CComPtr<IShellItem> result;
    hr = dialog->GetResult(&result);
    if (FAILED(hr)) { return hr; }

    CoTaskMemString path;
    hr = result->GetDisplayName(SIGDN_FILESYSPATH, path.Receive());
    if (FAILED(hr)) { return hr; }
    if (path.Get() == nullptr) { return E_UNEXPECTED; }

    outPath = path.Get();
    return S_OK;
}

bool IsUserCancelled(HRESULT hr) {
    return hr == HRESULT_FROM_WIN32(ERROR_CANCELLED);
}

bool ShellExecuteFile(HWND owner, LPCWSTR file, DWORD& outError) {
    outError = ERROR_SUCCESS;
    if (file == nullptr || *file == L'\0') {
        outError = ERROR_INVALID_PARAMETER;
        return false;
    }

    SHELLEXECUTEINFOW sei = {0};
    sei.cbSize = sizeof(sei);
    // NOASYNC: 呼び出し元が直後に終了しても起動処理が完了する。
    // FLAG_NO_UI: シェル既定のエラーダイアログを抑止し、エラー表示は呼び出し側に委ねる。
    sei.fMask  = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.hwnd   = owner;
    sei.lpFile = file;
    sei.nShow  = SW_SHOWNORMAL;

    if (::ShellExecuteExW(&sei)) { return true; }

    const DWORD err = ::GetLastError();
    outError = (err != ERROR_SUCCESS) ? err : ERROR_GEN_FAILURE;
    return false;
}

bool RevealInExplorer(HWND owner, LPCWSTR path, DWORD& outError) {
    outError = ERROR_SUCCESS;
    if (path == nullptr || *path == L'\0') {
        outError = ERROR_INVALID_PARAMETER;
        return false;
    }

    // `/ini:StirHex.ini` のような相対パスでも扱えるよう、まず絶対パスへ正規化する
    //   （相対パスは SHParseDisplayName でも解決できない。Issue #133）。
    const CStringW full = FullPath(path);

    // SHOpenFolderAndSelectItems はフォルダを開いた上で対象を選択状態にする。
    //   explorer.exe へ /select, 付きのコマンドラインを渡す方法と違い、パスの引用符や
    //   空白の扱いをシェルに委ねられる。
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(::SHParseDisplayName(full, nullptr, &pidl, 0, nullptr))) {
        const HRESULT hr = ::SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ::CoTaskMemFree(pidl);
        if (SUCCEEDED(hr)) { return true; }
    }

    // ここへ来るのは主に、設定ファイルがまだ書き出されていない初回起動。
    //   選択はできないが、置かれる場所は見せられる。
    //   相対ファイル名のように親フォルダ部分が無い場合は、保存先であるカレント
    //   ディレクトリを開く（Issue #133）。
    const std::wstring folder = stirling::path::FolderToReveal(
        std::wstring(static_cast<LPCWSTR>(full)),
        std::wstring(static_cast<LPCWSTR>(CurrentDirectory())));
    if (folder.empty()) {
        outError = ERROR_PATH_NOT_FOUND;
        return false;
    }
    return ShellExecuteFile(owner, folder.c_str(), outError);
}

CStringW FormatSystemError(DWORD error) {
    if (error == ERROR_SUCCESS) { return CStringW(); }

    LPWSTR buffer = nullptr;
    const DWORD len = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (len == 0 || buffer == nullptr) {
        ::LocalFree(buffer);
        return CStringW();
    }

    CStringW text(buffer, static_cast<int>(len));
    ::LocalFree(buffer);
    text.Trim(L" \t\r\n");
    return text;
}

CStringW AppendErrorReason(const CStringW& body, DWORD error) {
    const CStringW reason = FormatSystemError(error);
    if (reason.IsEmpty()) { return body; }

    CStringW detail;
    detail.Format(LoadW(IDS_ERROR_REASON), reason.GetString());
    return body + detail;
}

CStringW CurrentDirectory() {
    // 必要長を問い合わせてから確保する（長さには終端 NUL を含む）。
    const DWORD needed = ::GetCurrentDirectoryW(0, nullptr);
    if (needed == 0) { return CStringW(); }

    CStringW path;
    const DWORD written = ::GetCurrentDirectoryW(needed, path.GetBuffer(static_cast<int>(needed)));
    path.ReleaseBuffer((written < needed) ? static_cast<int>(written) : 0);
    return (written == 0 || written >= needed) ? CStringW() : path;
}

CStringW FullPath(LPCWSTR path) {
    if (path == nullptr || *path == L'\0') { return CStringW(); }

    // 必要長を問い合わせてから確保する（長さには終端 NUL を含む）。カレントディレクトリの
    //   変更で必要長が増える可能性があるため、確保が足りなければ数回だけ再試行する。
    for (int retry = 0; retry < 4; ++retry) {
        const DWORD needed = ::GetFullPathNameW(path, 0, nullptr, nullptr);
        if (needed == 0) { break; }

        CStringW full;
        wchar_t* buf = full.GetBuffer(static_cast<int>(needed));
        buf[0] = L'\0';   // 失敗時に未初期化バッファを ReleaseBuffer(0) で走査しないため
        const DWORD written = ::GetFullPathNameW(path, needed, buf, nullptr);
        full.ReleaseBuffer((written < needed) ? static_cast<int>(written) : 0);
        if (written == 0) { break; }
        if (written < needed) { return full; }
        // written >= needed: 問い合わせ後にパスが伸びた。再度必要長から取り直す。
    }
    return CStringW(path);   // 解決できない場合は入力をそのまま使う（原の挙動を維持）
}

CStringW DragQueryPath(HDROP drop, UINT index) {
    if (drop == nullptr) { return CStringW(); }

    // 必要長を問い合わせる（戻り値は終端 NUL を含まない文字数）。
    const UINT len = ::DragQueryFileW(drop, index, nullptr, 0);
    if (len == 0) { return CStringW(); }

    CStringW path;
    wchar_t* buf = path.GetBuffer(static_cast<int>(len));
    buf[0] = L'\0';
    const UINT written = ::DragQueryFileW(drop, index, buf, len + 1);
    path.ReleaseBuffer((written <= len) ? static_cast<int>(written) : 0);
    return (written == 0) ? CStringW() : path;
}

CStringW ModuleDirectory() {
    // GetModuleFileNameW には必要長の問い合わせが無いため、収まるまで倍々に広げる。
    //   上限は Win32 パスの最大長（32767 文字＋終端）。
    //   切り詰めの判定は「戻り値がバッファ長に達した」ことで行う。ERROR_INSUFFICIENT_BUFFER は
    //   Windows XP 世代では設定されないため、それだけに頼ると切り詰めたパスを成功扱いしてしまう。
    const int kMaxPathChars = 32768;
    for (int size = MAX_PATH;;) {
        CStringW path;
        ::SetLastError(ERROR_SUCCESS);
        // 切り詰め時は終端 NUL が無い場合があるため、長さは常に戻り値で明示する。
        const DWORD written = ::GetModuleFileNameW(nullptr, path.GetBuffer(size), size);
        const DWORD err = ::GetLastError();
        path.ReleaseBuffer(static_cast<int>(written));
        if (written == 0) { break; }

        if (written >= static_cast<DWORD>(size) || err == ERROR_INSUFFICIENT_BUFFER) {
            if (size >= kMaxPathChars) { break; }   // これ以上は広げられない
            size = (size * 2 < kMaxPathChars) ? (size * 2) : kMaxPathChars;
            continue;
        }

        const int slash = path.ReverseFind(L'\\');
        return (slash >= 0) ? path.Left(slash + 1) : CStringW();
    }
    return CStringW();
}

}  // namespace ui
