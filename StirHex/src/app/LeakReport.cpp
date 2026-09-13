// CRT リークレポートの出力（安定性評価。Issue #224）。
//
// Debug ビルドでのみ有効。環境変数 STIRHEX_LEAK_REPORT にファイルパスが設定されていると、
// プロセス終了時のリーク一覧をそのファイルへ書き出す。未設定なら何もしない。
// Release ビルドでは中身が丸ごと消え、リンクされるものは何も無い。
//
// ダンプのタイミング:
//   MFC は終了処理（AfxWinTerm）で CRT の自動リークダンプを落として自前のダンプを
//   デバッグ出力へ流すため、自動ダンプにも atexit にも頼れない。atexit ハンドラは
//   静的オブジェクトのデストラクタより前に走るので、その時点ではアプリの静的オブジェクト
//   （theApp とそのメンバ）が生きたままで、まだ解放されていない確保がリークとして並ぶ。
//   そこで #pragma init_seg(lib) で theApp より先に構築される静的オブジェクトを置く。
//   破棄は構築の逆順なので、この Reporter は最後に破棄され、そのデストラクタは
//   他の静的オブジェクトが解放された後に走る。
#include "pch.h"

#ifdef _DEBUG

#include <crtdbg.h>
#include <vector>

// アプリの静的オブジェクト（既定は init_seg(user)）より先に構築する＝最後に破棄する。
//   compiler ではなく lib を使う: compiler 相当の初期化は CRT の初期化より前に走るため、
//   その時点では operator new が使えずアクセス違反になる。
#pragma init_seg(lib)

namespace {

class LeakReporter {
public:
    LeakReporter() {
        const DWORD needed = ::GetEnvironmentVariableW(L"STIRHEX_LEAK_REPORT", nullptr, 0);
        if (needed == 0) {
            return;   // 未設定。既定の挙動（MFC のデバッグ出力）のまま
        }
        std::vector<wchar_t> path(needed);
        const DWORD written = ::GetEnvironmentVariableW(L"STIRHEX_LEAK_REPORT",
                                                        path.data(),
                                                        static_cast<DWORD>(path.size()));
        if (written == 0 || written >= path.size() || path[0] == L'\0') {
            return;
        }

        m_file = ::CreateFileW(path.data(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (m_file == INVALID_HANDLE_VALUE) {
            // 出力先を開けないときは既定の挙動へ戻す。リーク計測は補助機能であり、
            // ここで起動を止める理由にはならない。
            return;
        }
        _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_WARN, m_file);
    }

    ~LeakReporter() {
        if (m_file == INVALID_HANDLE_VALUE) {
            return;
        }
        _CrtDumpMemoryLeaks();
        ::CloseHandle(m_file);
        m_file = INVALID_HANDLE_VALUE;
    }

    LeakReporter(const LeakReporter&) = delete;
    LeakReporter& operator=(const LeakReporter&) = delete;

private:
    HANDLE m_file = INVALID_HANDLE_VALUE;
};

LeakReporter g_leakReporter;

}   // namespace

#endif   // _DEBUG
