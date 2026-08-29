// SettingsFile 実装（app/SettingsFile.h の規約を参照）。Win32 のみに依存する。
#include "app/SettingsFile.h"

#include <windows.h>
#include <shlobj.h>

#include <vector>

#include "util/ScopedHandle.h"

namespace stirling {
namespace settings {

const wchar_t kSettingsFileName[] = L"StirHex.ini";
const wchar_t kLegacyRegistryKey[] = L"Software\\StirHex\\StirHex";

namespace {

const wchar_t kAppDataFolderName[] = L"StirHex";

// GetLastError を「システムのメッセージ + コード」の形に整える。
std::wstring FormatLastError(const wchar_t* what, DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);

    std::wstring message(what);
    message += L": ";
    if (length != 0 && buffer != nullptr) {
        std::wstring text(buffer, length);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) { text.pop_back(); }
        message += text;
    } else {
        message += L"unknown error";
    }
    if (buffer != nullptr) { ::LocalFree(buffer); }
    message += L" (";
    message += std::to_wstring(code);
    message += L")";
    return message;
}

std::wstring DirectoryOf(const std::wstring& path) {
    const size_t sep = path.find_last_of(L"\\/");
    return (sep == std::wstring::npos) ? std::wstring() : path.substr(0, sep);
}

std::wstring ModulePath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD written = ::GetModuleFileNameW(nullptr, buffer.data(),
                                                   static_cast<DWORD>(buffer.size()));
        if (written == 0) { return std::wstring(); }
        if (written < buffer.size() - 1) { return std::wstring(buffer.data(), written); }
        if (buffer.size() >= 32768) { return std::wstring(); }
        buffer.resize(buffer.size() * 2);   // パスが長い場合に備えて広げて再試行する
    }
}

bool FileExists(const std::wstring& path) {
    if (path.empty()) { return false; }
    const DWORD attrs = ::GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// 親フォルダを作る（既存なら何もしない）。1階層だけ（%APPDATA%\StirHex を想定）。
bool EnsureParentDirectory(const std::wstring& path, std::wstring& error) {
    const std::wstring dir = DirectoryOf(path);
    if (dir.empty()) { return true; }
    const DWORD attrs = ::GetFileAttributesW(dir.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return true;
    }
    if (::CreateDirectoryW(dir.c_str(), nullptr)) { return true; }
    const DWORD code = ::GetLastError();
    if (code == ERROR_ALREADY_EXISTS) { return true; }
    error = FormatLastError((L"設定フォルダを作成できません: " + dir).c_str(), code);
    return false;
}

bool ReadWholeFile(const std::wstring& path, std::string& out, std::wstring& error) {
    // 共有は最大限許す。設定ファイルは他のプロセス（別インスタンス・テキストエディタ・
    //   テストのドライバ）からも読み書きされうるため、その最中でも読めるようにする。
    ScopedHandle file(::CreateFileW(path.c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.Valid()) {
        error = FormatLastError((L"設定ファイルを開けません: " + path).c_str(), ::GetLastError());
        return false;
    }
    LARGE_INTEGER size = { 0 };
    if (!::GetFileSizeEx(file.Get(), &size)) {
        error = FormatLastError((L"設定ファイルのサイズを取得できません: " + path).c_str(),
                                ::GetLastError());
        return false;
    }
    // 設定ファイルとしてありえない大きさは読まない（壊れたファイルで大量確保しない）。
    const long long kMaxBytes = 16LL * 1024 * 1024;
    if (size.QuadPart > kMaxBytes) {
        error = L"設定ファイルが大きすぎます: " + path;
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    while (offset < out.size()) {
        DWORD read = 0;
        const DWORD chunk = static_cast<DWORD>(
            (out.size() - offset) > 0x10000000u ? 0x10000000u : (out.size() - offset));
        if (!::ReadFile(file.Get(), out.data() + offset, chunk, &read, nullptr)) {
            error = FormatLastError((L"設定ファイルを読み込めません: " + path).c_str(),
                                    ::GetLastError());
            return false;
        }
        if (read == 0) { break; }
        offset += read;
    }
    out.resize(offset);
    return true;
}

bool WriteWholeFile(const std::wstring& path, const std::string& data, std::wstring& error) {
    ScopedHandle file(::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file.Valid()) {
        error = FormatLastError((L"設定ファイルを作成できません: " + path).c_str(),
                                ::GetLastError());
        return false;
    }
    size_t offset = 0;
    while (offset < data.size()) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(
            (data.size() - offset) > 0x10000000u ? 0x10000000u : (data.size() - offset));
        if (!::WriteFile(file.Get(), data.data() + offset, chunk, &written, nullptr)) {
            error = FormatLastError((L"設定ファイルを書き込めません: " + path).c_str(),
                                    ::GetLastError());
            return false;
        }
        offset += written;
    }
    if (!::FlushFileBuffers(file.Get())) {
        error = FormatLastError((L"設定ファイルを確定できません: " + path).c_str(),
                                ::GetLastError());
        return false;
    }
    return true;
}

// 一時ファイルで設定ファイルを置き換える。誰かが読んでいる瞬間に当たると共有違反で失敗
//   するため、短い間隔で数回だけ待って再試行する（設定の保存は稀なので待ち時間は目立たない）。
//   最後に失敗したときのエラーコードを code へ返す。
bool ReplaceWithRetry(const std::wstring& path, const std::wstring& temp, DWORD& code) {
    const int kAttempts = 5;
    const DWORD kWaitMs = 20;
    for (int i = 0; i < kAttempts; ++i) {
        if (::ReplaceFileW(path.c_str(), temp.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS,
                           nullptr, nullptr)) {
            return true;
        }
        code = ::GetLastError();
        if (code != ERROR_SHARING_VIOLATION && code != ERROR_ACCESS_DENIED &&
            code != ERROR_LOCK_VIOLATION) {
            return false;   // 一時的でない失敗は待っても変わらない
        }
        if (i + 1 < kAttempts) { ::Sleep(kWaitMs); }
    }
    return false;
}

// --- 複数インスタンスの同時保存（Issue #130） ---
//   同じ設定ファイルを使うプロセス同士を名前付きミューテックスで直列化する。名前には
//   パスをそのまま使えない（円記号はミューテックス名の名前空間区切り）ため、パスを
//   小文字化した FNV-1a ハッシュを使う。セッション内で足りるので Local 名前空間へ置く。
std::wstring SettingsMutexName(const std::wstring& path) {
    unsigned long long hash = 1469598103934665603ULL;   // FNV-1a 64bit offset basis
    for (wchar_t c : path) {
        wchar_t lower = c;
        if (lower >= L'A' && lower <= L'Z') { lower = static_cast<wchar_t>(lower - L'A' + L'a'); }
        if (lower == L'/') { lower = L'\\'; }   // 区切りの違いで別ロックにしない
        hash ^= static_cast<unsigned long long>(lower);
        hash *= 1099511628211ULL;
    }
    wchar_t digits[17] = {0};
    for (int i = 15; i >= 0; --i) {
        digits[i] = L"0123456789ABCDEF"[hash & 0xF];
        hash >>= 4;
    }
    return std::wstring(L"Local\\StirHex.Settings.") + digits;
}

// 保存の間だけ握るプロセス間ロック。取得できない環境でも保存自体は続行する
//   （ロックが無くても単独起動なら従来どおり動くため、設定を書けない方が困る）。
class SettingsFileLock {
public:
    explicit SettingsFileLock(const std::wstring& path)
        : mutex_(::CreateMutexW(nullptr, FALSE, SettingsMutexName(path).c_str())) {
        if (!mutex_.Valid()) { return; }
        const DWORD wait = ::WaitForSingleObject(mutex_.Get(), kWaitMs);
        // WAIT_ABANDONED: 直前の所有者が保存中に落ちた。所有権は得られているので続行する。
        held_ = (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED);
    }
    ~SettingsFileLock() {
        if (held_) { ::ReleaseMutex(mutex_.Get()); }
    }
    SettingsFileLock(const SettingsFileLock&) = delete;
    SettingsFileLock& operator=(const SettingsFileLock&) = delete;

private:
    static const DWORD kWaitMs = 5000;
    ScopedHandle mutex_;
    bool held_ = false;
};

// 一時ファイル名。プロセス毎に別名にして、同時保存で互いの一時ファイルを壊さない。
std::wstring TempPathFor(const std::wstring& path) {
    return path + L"." + std::to_wstring(::GetCurrentProcessId()) + L".tmp";
}

// レジストリ値1件を保存形式の文字列にする。扱えない型なら false。
bool RegValueToString(HKEY key, const std::wstring& name, DWORD type,
                      const std::vector<BYTE>& data, std::wstring& out) {
    (void)key;
    (void)name;
    switch (type) {
        case REG_DWORD: {
            if (data.size() < sizeof(DWORD)) { return false; }
            DWORD value = 0;
            ::memcpy(&value, data.data(), sizeof(value));
            // プロファイル API の整数値は符号付きとして読み書きする。
            out = std::to_wstring(static_cast<int>(value));
            return true;
        }
        case REG_SZ:
        case REG_EXPAND_SZ: {
            const size_t chars = data.size() / sizeof(wchar_t);
            std::wstring text(reinterpret_cast<const wchar_t*>(data.data()), chars);
            const size_t nul = text.find(L'\0');
            if (nul != std::wstring::npos) { text.resize(nul); }
            out = text;
            return true;
        }
        case REG_BINARY:
            out = BytesToHex(data.data(), data.size());
            return true;
        default:
            return false;
    }
}

// 1つのレジストリキーの値を全て store の section へ取り込む。取り込んだ件数を返す。
int ImportRegistryValues(HKEY key, const std::wstring& section, SettingsStore& store) {
    DWORD valueCount = 0;
    DWORD maxNameLen = 0;
    DWORD maxDataLen = 0;
    if (::RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                           &valueCount, &maxNameLen, &maxDataLen, nullptr,
                           nullptr) != ERROR_SUCCESS) {
        return 0;
    }
    std::vector<wchar_t> name(static_cast<size_t>(maxNameLen) + 1);
    std::vector<BYTE> data(static_cast<size_t>(maxDataLen) + 1);
    int imported = 0;
    for (DWORD i = 0; i < valueCount; ++i) {
        DWORD nameLen = static_cast<DWORD>(name.size());
        DWORD dataLen = static_cast<DWORD>(data.size());
        DWORD type = 0;
        const LSTATUS status = ::RegEnumValueW(key, i, name.data(), &nameLen, nullptr, &type,
                                               data.data(), &dataLen);
        if (status != ERROR_SUCCESS) { continue; }
        const std::wstring valueName(name.data(), nameLen);
        if (valueName.empty()) { continue; }   // 既定値（名前なし）はプロファイル API が使わない
        std::vector<BYTE> bytes(data.begin(), data.begin() + static_cast<ptrdiff_t>(dataLen));
        std::wstring text;
        if (!RegValueToString(key, valueName, type, bytes, text)) { continue; }
        store.Set(section, valueName, text);
        ++imported;
    }
    return imported;
}

}  // namespace

std::wstring FindCommandLineIniPath() {
    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv == nullptr) { return std::wstring(); }
    std::wstring result;
    for (int i = 1; i < argc && result.empty(); ++i) {
        const std::wstring arg(argv[i]);
        if (arg.size() <= 5) { continue; }
        if (arg[0] != L'/' && arg[0] != L'-') { continue; }
        // `/ini:` の判定は大文字小文字を区別しない。
        if ((arg[1] == L'i' || arg[1] == L'I') &&
            (arg[2] == L'n' || arg[2] == L'N') &&
            (arg[3] == L'i' || arg[3] == L'I') &&
            arg[4] == L':') {
            result = arg.substr(5);
        }
    }
    ::LocalFree(argv);
    return result;
}

std::wstring ExeDirSettingsPath() {
    const std::wstring dir = DirectoryOf(ModulePath());
    if (dir.empty()) { return std::wstring(); }
    return dir + L"\\" + kSettingsFileName;
}

std::wstring AppDataSettingsPath() {
    wchar_t* roaming = nullptr;
    if (FAILED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roaming)) ||
        roaming == nullptr) {
        if (roaming != nullptr) { ::CoTaskMemFree(roaming); }
        return std::wstring();
    }
    std::wstring path(roaming);
    ::CoTaskMemFree(roaming);
    path += L"\\";
    path += kAppDataFolderName;
    path += L"\\";
    path += kSettingsFileName;
    return path;
}

SettingsLocation ResolveSettingsLocation() {
    SettingsLocation location;

    const std::wstring fromCmdLine = FindCommandLineIniPath();
    if (!fromCmdLine.empty()) {
        location.path = fromCmdLine;
        location.source = SettingsSource::CommandLine;
        return location;
    }

    const std::wstring portable = ExeDirSettingsPath();
    if (FileExists(portable)) {
        location.path = portable;
        location.source = SettingsSource::PortableExeDir;
        return location;
    }

    location.path = AppDataSettingsPath();
    location.source = SettingsSource::AppData;
    return location;
}

bool SettingsFileExists(const std::wstring& path) {
    return FileExists(path);
}

bool ReadTextFileUtf8(const std::wstring& path, std::wstring& text, std::wstring& error) {
    error.clear();
    text.clear();

    std::string bytes;
    if (!ReadWholeFile(path, bytes, error)) { return false; }
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }
    if (!Utf8ToWide(bytes, text)) {
        error = L"文字コードが UTF-8 として不正です: " + path;
        return false;
    }
    return true;
}

bool WriteTextFileUtf8(const std::wstring& path, const std::wstring& text,
                       std::wstring& error) {
    error.clear();
    // 設定ファイルと違い、書き込み先は利用者が選んだ場所そのもの。一時ファイルを
    //   経由すると選んだ場所に別名のごみが残りうるため、直接書く。
    return WriteWholeFile(path, WideToUtf8(text), error);
}

bool LoadSettingsFile(const std::wstring& path, SettingsStore& store, std::wstring& error) {
    error.clear();
    if (path.empty()) {
        error = L"設定ファイルの保存先を決定できません";
        return false;
    }
    if (!FileExists(path)) { return true; }   // 初回起動。既定値のまま進む

    std::string bytes;
    if (!ReadWholeFile(path, bytes, error)) { return false; }

    // BOM は書かないが、利用者が BOM 付きで保存した場合も読めるようにする。
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }

    std::wstring text;
    const bool decoded = Utf8ToWide(bytes, text);
    const bool parsed = store.ParseInto(text);
    if (!decoded) {
        error = L"設定ファイルの文字コードが UTF-8 として不正です: " + path;
        return false;
    }
    if (!parsed) {
        error = L"設定ファイルに解釈できない行があります: " + path;
        return false;
    }
    return true;
}

bool SaveSettingsFile(const std::wstring& path, const SettingsStore& store, std::wstring& error) {
    error.clear();
    if (path.empty()) {
        error = L"設定ファイルの保存先を決定できません";
        return false;
    }
    if (!EnsureParentDirectory(path, error)) { return false; }

    const std::string data = WideToUtf8(store.Serialize());

    // 一時ファイルへ書いてから置換する（書き込み途中で落ちても既存ファイルを壊さない）。
    //   名前はプロセス毎に変える（同時保存で一時ファイルが競合しないように。Issue #130）。
    const std::wstring temp = TempPathFor(path);
    if (!WriteWholeFile(temp, data, error)) {
        ::DeleteFileW(temp.c_str());
        return false;
    }
    if (FileExists(path)) {
        DWORD code = 0;
        if (!ReplaceWithRetry(path, temp, code)) {
            error = FormatLastError((L"設定ファイルを置き換えられません: " + path).c_str(), code);
            ::DeleteFileW(temp.c_str());
            return false;
        }
        return true;
    }
    if (!::MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = FormatLastError((L"設定ファイルを配置できません: " + path).c_str(),
                                ::GetLastError());
        ::DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

// 最新のファイル内容へ、このプロセスの変更だけを適用して書き戻す（Issue #130）。
//   プロセス毎の古いスナップショット全体で置換すると、先に終了した別プロセスの更新を
//   古い値で上書きしてしまう。保存の間はプロセス間ロックで直列化する。
bool SaveSettingsFileMerged(const std::wstring& path, SettingsStore& store, std::wstring& error) {
    error.clear();
    if (path.empty()) {
        error = L"設定ファイルの保存先を決定できません";
        return false;
    }
    if (store.Changes().empty()) {
        store.ClearDirty();
        return true;   // 書くものが無い
    }

    const SettingsFileLock lock(path);

    // ロックの中で読み直す。読めないファイルは壊さない（呼び元が利用者へ知らせる）。
    SettingsStore merged;
    if (!LoadSettingsFile(path, merged, error)) { return false; }
    merged.ApplyChanges(store.Changes());

    if (!SaveSettingsFile(path, merged, error)) { return false; }
    store.ClearDirty();   // 書けた分の記録を落とす（以降の変更だけを次回マージする）
    return true;
}

bool ImportFromRegistry(const wchar_t* subKey, SettingsStore& store) {
    if (subKey == nullptr) { return false; }
    HKEY root = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, subKey, 0, KEY_READ, &root) != ERROR_SUCCESS) {
        return false;
    }

    int imported = 0;
    DWORD subKeyCount = 0;
    DWORD maxSubKeyLen = 0;
    if (::RegQueryInfoKeyW(root, nullptr, nullptr, nullptr, &subKeyCount, &maxSubKeyLen, nullptr,
                           nullptr, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        std::vector<wchar_t> name(static_cast<size_t>(maxSubKeyLen) + 1);
        for (DWORD i = 0; i < subKeyCount; ++i) {
            DWORD nameLen = static_cast<DWORD>(name.size());
            if (::RegEnumKeyExW(root, i, name.data(), &nameLen, nullptr, nullptr, nullptr,
                                nullptr) != ERROR_SUCCESS) {
                continue;
            }
            const std::wstring section(name.data(), nameLen);
            HKEY child = nullptr;
            if (::RegOpenKeyExW(root, section.c_str(), 0, KEY_READ, &child) != ERROR_SUCCESS) {
                continue;
            }
            imported += ImportRegistryValues(child, section, store);
            ::RegCloseKey(child);
        }
    }
    ::RegCloseKey(root);
    return imported > 0;
}

}  // namespace settings
}  // namespace stirling
