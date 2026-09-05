// CMainFrame 実装。動的ツールバー／ステータスバーと各ドッキングペインを管理する。
#include "pch.h"
#include "app/UiStrings.h"   // UI文字列はリソースから
#include "frame/MainFrame.h"
#include "dialog/BgrepDlg.h"
#include "dialog/DiffListDlg.h"
#include "dialog/BgrepStatusDlg.h"
#include "dialog/ExtSettingsDlg.h"
#include "dialog/EnvSettingsDlg.h"
#include "dialog/RunDlg.h"
#include "frame/ToolbarCatalog.h"
#include "doc/StirlingDoc.h"
#include "view/StirlingView.h"
#include "app/StirlingApp.h"
#include "app/ShellUtil.h"   // ui::DragQueryPath（MAX_PATH 非依存のドロップパス取得）
#include "resource.h"

#include <afxpriv.h>   // CDockBar（ドッキング先の判定。Issue #121）

#include <vector>

IMPLEMENT_DYNAMIC(CMainFrame, CMDIFrameWnd)

namespace {
// ビットイメージのリアルタイム反映用の遅延再構築メッセージ（他の WM_APP 使用と重複しない値）。
const UINT kBitImageRefreshMsg = WM_APP + 0x131;

bool WidePathToMbc(const wchar_t* path, int length, CString& out) {
    if (path == nullptr || length <= 0) return false;
#ifdef _UNICODE
    out.SetString(path, length);
    return true;
#else
    const int bytes = ::WideCharToMultiByte(CP_ACP, 0, path, length,
                                             nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return false;
    ::WideCharToMultiByte(CP_ACP, 0, path, length,
                          out.GetBuffer(bytes), bytes, nullptr, nullptr);
    out.ReleaseBuffer(bytes);
    return true;
#endif
}

bool DecodeTransferredFiles(const COPYDATASTRUCT* copyData, std::vector<CString>& files) {
    files.clear();
    if (copyData == nullptr ||
        copyData->dwData != stirling::single_instance::kCopyDataId ||
        copyData->lpData == nullptr ||
        copyData->cbData < sizeof(wchar_t) * 2 ||
        (copyData->cbData % sizeof(wchar_t)) != 0) {
        return false;
    }
    const wchar_t* data = static_cast<const wchar_t*>(copyData->lpData);
    const size_t count = copyData->cbData / sizeof(wchar_t);
    if (data[count - 1] != L'\0' || data[count - 2] != L'\0') return false;

    size_t offset = 0;
    while (offset < count - 1 && data[offset] != L'\0') {
        const size_t remaining = count - offset;
        const size_t length = wcsnlen(data + offset, remaining);
        if (length == remaining) return false;
        CString path;
        if (!WidePathToMbc(data + offset, static_cast<int>(length), path)) return false;
        files.push_back(path);
        offset += length + 1;
    }
    return !files.empty();
}
}

BEGIN_MESSAGE_MAP(CMainFrame, CMDIFrameWnd)
    ON_WM_CREATE()
    ON_WM_DESTROY()
    ON_WM_COPYDATA()
    ON_WM_SIZE()
    ON_WM_CLOSE()       // 終了時に winPlacement=1 の配置保存
    ON_WM_ENDSESSION()  // ログオフ／シャットダウンは OnClose を通らない（Issue #121）
    ON_WM_DROPFILES()   // D&D はリンク解決せず開く（MFC既定 OnDropFiles を上書き）
    // 構造体編集バー トグル: 編集(0x802e)・設定(0x80f6) 両メニューとも同一動作（原と一致）。
    ON_COMMAND(ID_STRUCT_EDIT, &CMainFrame::OnStructBarToggle)
    ON_UPDATE_COMMAND_UI(ID_STRUCT_EDIT, &CMainFrame::OnUpdateStructBarToggle)
    ON_COMMAND(ID_STRUCT_EDIT_TOGGLE, &CMainFrame::OnStructBarToggle)
    ON_UPDATE_COMMAND_UI(ID_STRUCT_EDIT_TOGGLE, &CMainFrame::OnUpdateStructBarToggle)
    // 「キャレット位置を構造体編集」（0x8061）。原はフレーム経由のコマンドとして処理する。
    ON_COMMAND(ID_STRUCT_CARET, &CMainFrame::OnStructBarCaret)
    // 名前を指定して実行（0x804f）。文書が無くても実行できる（原実測）。
    ON_COMMAND(ID_RUN_APP, &CMainFrame::OnRunApp)
    // ビットイメージのリアルタイム反映（遅延再構築）。
    ON_MESSAGE(kBitImageRefreshMsg, &CMainFrame::OnBitImageRefreshQueued)
    // アウトプットペイン（トグル 0x80e8／タグジャンプ 0x80ea／コピー 0x80f7／クリア 0x80e9）。
    ON_COMMAND(ID_OUTPUT_PANE, &CMainFrame::OnOutputPaneToggle)
    ON_UPDATE_COMMAND_UI(ID_OUTPUT_PANE, &CMainFrame::OnUpdateOutputPaneToggle)
    ON_COMMAND(ID_TAG_JUMP, &CMainFrame::OnOutputTagJump)
    ON_UPDATE_COMMAND_UI(ID_TAG_JUMP, &CMainFrame::OnUpdateOutputHasResults)
    ON_COMMAND(ID_SEARCH_RESULT_COPY, &CMainFrame::OnOutputCopy)
    ON_UPDATE_COMMAND_UI(ID_SEARCH_RESULT_COPY, &CMainFrame::OnUpdateOutputHasResults)
    ON_COMMAND(ID_OUTPUT_CLEAR, &CMainFrame::OnOutputClear)
    ON_UPDATE_COMMAND_UI(ID_OUTPUT_CLEAR, &CMainFrame::OnUpdateOutputHasResults)
    ON_COMMAND(ID_BGREP, &CMainFrame::OnBgrep)
    // ビットイメージ・ペイン（トグル 0x80eb／最新イメージ 0x80ec）。
    ON_COMMAND(ID_BITIMAGE, &CMainFrame::OnBitImageToggle)
    ON_UPDATE_COMMAND_UI(ID_BITIMAGE, &CMainFrame::OnUpdateBitImageToggle)
    ON_COMMAND(ID_BITIMAGE_LATEST, &CMainFrame::OnBitImageLatest)
    // 環境設定（0x8050）。アプリ全体の動作環境をプロパティシートで編集→OKで保存。
    ON_COMMAND(ID_SETTINGS_ENV, &CMainFrame::OnSettingsEnv)
    // 拡張子別設定（0x8051）。一覧で拡張子レコードを編集し、全ビューへ再適用＋保存。
    ON_COMMAND(ID_SETTINGS_EXT, &CMainFrame::OnSettingsExt)
    // ステータスバー各ペインのフォールバック更新（未オープン時に空欄化。ビュー活性時はビュー側が処理）。
    //   カタログ全20項目のうち Caps/Num/Scroll ロック(0xE701-0xE703)は MFC 標準処理に委ねる。
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_MODE,      &CMainFrame::OnUpdateIndicatorEmpty)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_EDITLOCK,  &CMainFrame::OnUpdateIndicatorEmpty)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_MODIFIED,  &CMainFrame::OnUpdateIndicatorEmpty)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_ADDRESS,   &CMainFrame::OnUpdateIndicatorEmpty)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_SIZE,      &CMainFrame::OnUpdateIndicatorEmpty)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_WORD_HEX,  &CMainFrame::OnUpdateIndicatorEmpty)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_DWORD_HEX, &CMainFrame::OnUpdateIndicatorEmpty)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_CHARSET,   &CMainFrame::OnUpdateIndicatorEmpty)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_ADDR_DEC,  &CMainFrame::OnUpdateIndicatorEmpty)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_BYTE_DEC,  &CMainFrame::OnUpdateIndicatorEmpty)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_WORD_DEC,  &CMainFrame::OnUpdateIndicatorEmpty)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_DWORD_DEC, &CMainFrame::OnUpdateIndicatorEmpty)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_SIZE_HEX,  &CMainFrame::OnUpdateIndicatorEmpty)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_BYTE_HEX,  &CMainFrame::OnUpdateIndicatorEmpty)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_FLOAT,     &CMainFrame::OnUpdateIndicatorEmpty)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_DOUBLE,    &CMainFrame::OnUpdateIndicatorEmpty)
    ON_UPDATE_COMMAND_UI(ID_INDICATOR_BYTEORDER, &CMainFrame::OnUpdateIndicatorEmpty)
END_MESSAGE_MAP()

CMainFrame::CMainFrame() {}

CMainFrame::~CMainFrame() {}

// タイトル末尾に編集マーク「 *」を付与（原挙動: 活性文書が変更あり時。
//   例 "Stirling - stir131.lzh *"）。
void CMainFrame::OnUpdateFrameTitle(BOOL bAddToTitle) {
    CMDIFrameWnd::OnUpdateFrameTitle(bAddToTitle);   // 通常のタイトル（アプリ名 - ファイル名）
    // MDIメインフレームは自身のビューを持たないため GetActiveDocument() は NULL。
    // 活性 MDI 子から活性文書を取得する。
    CDocument* pDoc = nullptr;
    if (CMDIChildWnd* pActive = MDIGetActive()) {
        pDoc = pActive->GetActiveDocument();
    }
    if (pDoc != nullptr && pDoc->IsModified()) {
        CString title;
        GetWindowText(title);
        if (!title.IsEmpty() && title.Right(2) != _T(" *")) {
            SetWindowText(title + _T(" *"));
        }
    }
}

// ステータスバー・ペインのフォールバック更新。ビュー活性時はコマンド経路がビュー側の
//   OnUpdateIndicator* で処理され、ここには到達しない。文書未オープン時のみ到達するので、
//   ペインを無効化してテンプレート文字列（幅算出用の 0x…/…Bytes/文字セット名）を消す。
void CMainFrame::OnUpdateIndicatorEmpty(CCmdUI* pCmdUI) {
    pCmdUI->Enable(FALSE);
    pCmdUI->SetText(_T(""));
}

int CMainFrame::OnCreate(LPCREATESTRUCT lpCreateStruct) {
    if (CMDIFrameWnd::OnCreate(lpCreateStruct) == -1) {
        return -1;
    }
    if (!::SetPropW(GetSafeHwnd(), stirling::single_instance::kMainFrameProperty,
                    reinterpret_cast<HANDLE>(stirling::single_instance::kMainFrameMagic))) {
        AfxMessageBox(ui::LoadW(IDS_ERR_MAINWND_ID),
                      MB_OK | MB_ICONERROR);
        return -1;
    }
    // メインツールバー（StirHex 独自生成リソース128。ボタン列は toolbarItems から動的構築）。
    if (!m_wndToolBar.CreateEx(this, TBSTYLE_FLAT,
            WS_CHILD | WS_VISIBLE | CBRS_TOP | CBRS_GRIPPER | CBRS_TOOLTIPS |
            CBRS_FLYBY | CBRS_SIZE_DYNAMIC) ||
        !m_wndToolBar.LoadBitmap(IDR_MAINFRAME)) {
        return -1;
    }
    m_wndToolBar.SetSizes(CSize(23, 22), CSize(16, 15));   // 原ビットマップの画像サイズに合わせる
    RebuildToolbar();
    if (!m_wndStatusBar.Create(this)) {
        return -1;
    }
    RebuildStatusBar();   // statusItems からペイン列を動的構築（原の動的構築相当）
    // 構造体編集バー（上/下ドッキングまたはフローティング。既定非表示。
    //   0x802e/0x80f6 でトグル）。
    if (!m_wndStructBar.CreateBar(this)) {
        return -1;
    }
    // ツールバーを上端にドッキング。
    m_wndToolBar.EnableDocking(CBRS_ALIGN_ANY);
    EnableDocking(CBRS_ALIGN_ANY);
    DockControlBar(&m_wndToolBar);
    ApplyStructBarSettings();
    // アウトプットペイン（上下ドッキング／フローティング。初期は下端100px。
    //   原版同様、配置は起動ごとに初期化。0x80e8 でトグル）。
    //   表示状態のみ設定から復元する（既定は非表示。Issue #148）。
    if (!m_wndOutputBar.CreateBar(this)) {
        return -1;
    }
    DockControlBar(&m_wndOutputBar, AFX_IDW_DOCKBAR_BOTTOM);
    ShowControlBar(&m_wndOutputBar, theApp.AppSettings().showOutputPane, FALSE);
    // ビットイメージ・ペイン（起動配置確定後にフローティング。既定非表示。0x80eb でトグル）。
    //   生成直後はMFCの初期化用として左ドックへ仮配置する。
    if (!m_wndBitImageBar.CreateBar(this)) {
        return -1;
    }
    DockControlBar(&m_wndBitImageBar, AFX_IDW_DOCKBAR_LEFT);
    ShowControlBar(&m_wndBitImageBar, FALSE, FALSE);   // 既定は非表示
    // ツールバー/ステータスバーの表示状態を環境設定に従わせる（原の起動時反映）。
    ShowControlBar(&m_wndToolBar,   theApp.AppSettings().showToolbar,   FALSE);
    ShowControlBar(&m_wndStatusBar, theApp.AppSettings().showStatusbar, FALSE);
    // ファイルのドラッグ＆ドロップでオープンを受け付ける（原 shell32 DragAcceptFiles/
    //   DragQueryFileA）。フレーム領域(ツールバー/空のMDIクライアント等)のドロップは
    //   下の OnDropFiles、ドキュメント欄のドロップはビュー側で処理。いずれもリンク解決しない。
    DragAcceptFiles(TRUE);
    return 0;
}

void CMainFrame::OnDestroy() {
    ::RemovePropW(GetSafeHwnd(), stirling::single_instance::kMainFrameProperty);
    CMDIFrameWnd::OnDestroy();
}

BOOL CMainFrame::OnCopyData(CWnd* pWnd, COPYDATASTRUCT* pCopyDataStruct) {
    std::vector<CString> files;
    if (!DecodeTransferredFiles(pCopyDataStruct, files)) {
        return CMDIFrameWnd::OnCopyData(pWnd, pCopyDataStruct);
    }

    bool opened = true;
    for (const CString& path : files) {
        if (theApp.OpenDroppedFile(path) == nullptr) {
            opened = false;
            break;
        }
    }
    if (IsIconic()) ShowWindow(SW_RESTORE);
    ActivateFrame();
    SetForegroundWindow();
    return opened ? TRUE : FALSE;
}

// フレームのサイズ変更とコントロールバーの再配置は、どちらもMDIクライアント領域を変える。
//   最小化プロキシがクライアント外へ隠れて操作不能にならないよう、ここで追従させる（Issue #123）。
void CMainFrame::RecalcLayout(BOOL bNotify) {
    CMDIFrameWnd::RecalcLayout(bNotify);
    CDiffListDlg::RepositionMinimizedProxies();
}

void CMainFrame::OnSize(UINT nType, int cx, int cy) {
    CMDIFrameWnd::OnSize(nType, cx, cy);
    if (m_wndStructBar.GetSafeHwnd() != nullptr) {
        m_wndStructBar.FitToFrame();
    }
}

// 終了時の状態保存（通常の終了経路）。
void CMainFrame::OnClose() {
    SaveWindowStateToSettings();
    CMDIFrameWnd::OnClose();
}

// ログオフ／シャットダウン時の状態保存。CFrameWnd::OnEndSession（winfrm.cpp）は
//   OnClose を経由せず CWinApp::ExitInstance() を直接呼ぶため、そこで設定が保存される
//   前にここで書き戻す。これが無いと、最後にメニューを開いてから動かした位置など、
//   コマンド経由で同期されない変更が失われる（Issue #121）。
void CMainFrame::OnEndSession(BOOL bEnding) {
    if (bEnding) {
        SaveWindowStateToSettings();
    }
    CMDIFrameWnd::OnEndSession(bEnding);
}

// 終了時に画面の状態を設定へ書き戻す。OnClose と OnEndSession の共通処理。
//   winPlacement=1(前回終了時)なら現在の通常位置・サイズを保存する（原ヘルプ準拠）。
//   最大化中でも rcNormalPosition（復元時の矩形）を保存し、次回は SW_SHOWNORMAL で復元する（原と一致）。
void CMainFrame::SaveWindowStateToSettings() {
    // アウトプットペインの表示状態を書き戻す（原 FUN_00426e4c と同じ粒度。Issue #148）。
    SyncOutputPaneSetting();
    // ビットイメージの表示状態と配置を書き戻す（移植独自。Issue #121）。
    SyncBitImageSetting();
    if (theApp.AppSettings().winPlacement == 1) {
        WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
        if (GetWindowPlacement(&wp)) {
            CAppSettings& s = theApp.AppSettings();
            s.winLeft   = wp.rcNormalPosition.left;
            s.winTop    = wp.rcNormalPosition.top;
            s.winWidth  = wp.rcNormalPosition.right  - wp.rcNormalPosition.left;
            s.winHeight = wp.rcNormalPosition.bottom - wp.rcNormalPosition.top;
        }
    }
}

// フレーム領域へのファイルドロップ。原ヘルプ準拠でリンク解決せず開く（MFC既定
//   CFrameWnd::OnDropFiles は CDocManager 経由でショートカットを解決するため上書き）。
void CMainFrame::OnDropFiles(HDROP hDropInfo) {
    const UINT count = ::DragQueryFile(hDropInfo, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        const CStringW path = ui::DragQueryPath(hDropInfo, i);
        if (!path.IsEmpty()) {
            theApp.OpenDroppedFile(path);
        }
    }
    ::DragFinish(hDropInfo);
}

// toolbarItems（アプリ設定）からツールバーのボタン列を再構築する（原の動的構築相当）。
//   セパレータ(0xFFFF)→TBBS_SEPARATOR。機能→実コマンドID＋独自ビットマップの画像索引。
//   コマンド/画像が未定義の項目はセパレータとして無視する（安全側）。
void CMainFrame::RebuildToolbar() {
    if (m_wndToolBar.GetSafeHwnd() == nullptr) { return; }
    const std::vector<UINT>& items = theApp.AppSettings().toolbarItems;
    const int count = items.empty() ? 1 : (int)items.size();
    m_wndToolBar.SetButtons(nullptr, count);   // 全スロットをセパレータで確保
    for (int i = 0; i < (int)items.size(); ++i) {
        const UINT raw = items[i];
        if (raw == CAppSettings::kToolbarSep) {
            m_wndToolBar.SetButtonInfo(i, ID_SEPARATOR, TBBS_SEPARATOR, 8);
            continue;
        }
        const UINT cmd = ToolbarRawToCmd(raw);
        const int  img = ToolbarRawToImage(raw);
        if (cmd == 0 || img < 0) {
            m_wndToolBar.SetButtonInfo(i, ID_SEPARATOR, TBBS_SEPARATOR, 8);   // 未定義は無視
        } else {
            m_wndToolBar.SetButtonInfo(i, cmd, TBBS_BUTTON, img);
        }
    }
}

// statusItems（アプリ設定）からステータスバーのペイン列を再構築する（原の動的構築相当）。
//   先頭はメッセージ行（ID_SEPARATOR）。各項目ID(0xE7xx)は SetIndicators が対応するRC文字列
//   (59136-59158＝幅テンプレート)を読み込み、初期テキスト兼ペイン幅として設定する。
//   実際の値は活性ビューの ON_UPDATE_COMMAND_UI が毎アイドルで更新する。
void CMainFrame::RebuildStatusBar() {
    if (m_wndStatusBar.GetSafeHwnd() == nullptr) { return; }
    const std::vector<UINT>& items = theApp.AppSettings().statusItems;
    std::vector<UINT> ind;
    ind.reserve(items.size() + 1);
    ind.push_back(ID_SEPARATOR);   // メッセージ行（レディ / コマンド説明。伸縮ペイン）
    for (UINT id : items) {
        ind.push_back(id);
    }
    m_wndStatusBar.SetIndicators(ind.data(), (int)ind.size());
}

// 環境設定変更後にツールバー/ステータスバー構成と表示状態を再適用する。
void CMainFrame::ApplyBarSettings() {
    RebuildToolbar();
    RebuildStatusBar();
    ShowControlBar(&m_wndToolBar,   theApp.AppSettings().showToolbar,   FALSE);
    ShowControlBar(&m_wndStatusBar, theApp.AppSettings().showStatusbar, FALSE);
    ApplyStructBarSettings();
    ApplyBitImageBarSettings();
    RecalcLayout();
}

void CMainFrame::ApplyBitImageBarSettings() {
    if (m_wndBitImageBar.GetSafeHwnd() == nullptr) return;
    // 原 FUN_00425b01: ON=0x5000（左右）/ OFF=0 を EnableDocking へ渡す。
    m_wndBitImageBar.SetDockable(theApp.AppSettings().bitImageDockable);
}

// ビットイメージの既定フローティング位置（原版同様、本体ウィンドウの左隣）。
CPoint CMainFrame::DefaultBitImageFloatPoint() const {
    CRect frameRect;
    // 最小化中の GetWindowRect はアイコン位置（Y=-32000 等）を返すため、
    //   復元時の矩形を使う。最小化状態で起動された場合でも画面内に置く（Issue #121）。
    if (IsIconic()) {
        WINDOWPLACEMENT wp = { sizeof(WINDOWPLACEMENT) };
        if (GetWindowPlacement(&wp)) {
            frameRect = wp.rcNormalPosition;
        } else {
            GetWindowRect(&frameRect);
        }
    } else {
        GetWindowRect(&frameRect);
    }
    const CSize floatSize = CBitImageBar::DefaultFloatingSize();
    const int frameEdge = ::GetSystemMetrics(SM_CXFRAME);
    return CPoint(max(0, frameRect.left - floatSize.cx - frameEdge), frameRect.top);
}

// 保存されたフローティング位置を返す。未保存の場合や、前回から画面構成が
//   変わってどのモニタにもかからなくなった場合は、既定位置へフォールバックする
//   （メインウィンドウの配置復元と同じ考え方。Issue #121）。
CPoint CMainFrame::RestoredBitImageFloatPoint() const {
    const CAppSettings& s = theApp.AppSettings();
    if (s.bitImageLeft == CAppSettings::kBitImagePosUnset ||
        s.bitImageTop  == CAppSettings::kBitImagePosUnset) {
        return DefaultBitImageFloatPoint();
    }
    const CSize floatSize = CBitImageBar::DefaultFloatingSize();
    const CRect saved(CPoint(s.bitImageLeft, s.bitImageTop), floatSize);
    if (::MonitorFromRect(&saved, MONITOR_DEFAULTTONULL) == nullptr) {
        return DefaultBitImageFloatPoint();
    }
    return saved.TopLeft();
}

// メインウィンドウの配置確定後に、ビットイメージの配置と表示状態を確定する。
//   原版はここで常に「非表示・本体左隣のフローティング」へ初期化するが、移植版は
//   前回の状態を引き継ぐ（Issue #121）。
void CMainFrame::FinalizeInitialBitImagePlacement() {
    if (m_wndBitImageBar.GetSafeHwnd() == nullptr) return;

    const CAppSettings& s = theApp.AppSettings();
    // DockControlBar / FloatControlBar 実行前はいったんドッキングを許可する。
    m_wndBitImageBar.SetDockable(true);

    // ドッキング位置の復元は bitImageDockable が ON のときだけ。保存後に
    //   設定を OFF へ変えた場合は、ドッキングできない以上フローティングへ戻す。
    const bool restoreDocked = s.bitImageDockable && s.bitImagePlacement != 0;
    if (restoreDocked) {
        DockControlBar(&m_wndBitImageBar,
                       s.bitImagePlacement == 1 ? AFX_IDW_DOCKBAR_LEFT : AFX_IDW_DOCKBAR_RIGHT);
    } else {
        FloatControlBar(&m_wndBitImageBar, RestoredBitImageFloatPoint(), CBRS_ALIGN_TOP);
    }

    // 原版同様、初期配置後に設定値でドッキング可否だけを切り替える。
    ApplyBitImageBarSettings();
    ShowControlBar(&m_wndBitImageBar, s.bitImageShow, FALSE);
    if (s.bitImageShow) {
        m_wndBitImageBar.Refresh(ActiveStirlingDoc());
    }
}

// ビットイメージの現在の表示状態と配置を設定へ取り込む（Issue #121）。
//   同期する箇所はアウトプットペイン（SyncOutputPaneSetting）と揃える。終了が OnClose を
//   通らない経路（ログオフ／シャットダウン）や、環境設定の OK が設定一式を保存する
//   場合にも、保存される値を最新に保つため。
void CMainFrame::SyncBitImageSetting() {
    if (m_wndBitImageBar.GetSafeHwnd() == nullptr) { return; }

    CAppSettings& s = theApp.AppSettings();
    s.bitImageShow = (m_wndBitImageBar.IsWindowVisible() != FALSE);

    if (m_wndBitImageBar.IsFloating()) {
        s.bitImagePlacement = 0;
        // フローティング中のバーはミニフレームの子。位置はその枠ごとの左上を使う。
        //   非表示中でもミニフレーム自体は存在するため位置を取得できる。
        CFrameWnd* miniFrame = m_wndBitImageBar.GetParentFrame();
        if (miniFrame != nullptr && miniFrame != this) {
            CRect rect;
            miniFrame->GetWindowRect(&rect);
            s.bitImageLeft = rect.left;
            s.bitImageTop  = rect.top;
        }
        return;
    }

    // ドッキング中は左右のどちらか（CBitImageBar は左右のみ許可）。
    //   フローティング位置は次にフローティングへ戻したときのために前回値を残す。
    if (m_wndBitImageBar.m_pDockBar != nullptr) {
        s.bitImagePlacement =
            (m_wndBitImageBar.m_pDockBar->GetDlgCtrlID() == AFX_IDW_DOCKBAR_RIGHT) ? 2 : 1;
    }
}

void CMainFrame::ApplyStructBarSettings() {
    if (m_wndStructBar.GetSafeHwnd() == nullptr) return;

    const CAppSettings& settings = theApp.AppSettings();
    const BOOL wasVisible = m_wndStructBar.IsWindowVisible();
    const DWORD dockSides = CBRS_ALIGN_TOP | CBRS_ALIGN_BOTTOM;

    // 以前「ドッキング不能」だった場合でも、再配置前はいったんドッキングを許可する。
    m_wndStructBar.EnableDocking(dockSides);
    if (settings.structBarPos == 0) {
        DockControlBar(&m_wndStructBar, AFX_IDW_DOCKBAR_BOTTOM);
    } else if (settings.structBarPos == 1) {
        DockControlBar(&m_wndStructBar, AFX_IDW_DOCKBAR_TOP);
    } else {
        // 既にフローティングなら現在位置を保ち、初回だけ原版同様に右上寄りへ置く。
        CPoint floatPoint;
        CFrameWnd* parentFrame = m_wndStructBar.GetParentFrame();
        if (parentFrame != nullptr && parentFrame != this) {
            CRect rect;
            parentFrame->GetWindowRect(&rect);
            floatPoint = rect.TopLeft();
        } else {
            CRect frameRect;
            GetWindowRect(&frameRect);
            floatPoint.x = max(frameRect.left, frameRect.right - 580);
            floatPoint.y = frameRect.top + 60;
        }
        FloatControlBar(&m_wndStructBar, floatPoint, CBRS_ALIGN_TOP);
        m_wndStructBar.EnableDocking(settings.structBarNoDock ? 0 : dockSides);
    }

    m_wndStructBar.ApplyDisplaySettings(settings.structBarStatusPos,
                                        settings.structItemRatioKeep);
    ShowControlBar(&m_wndStructBar, wasVisible, FALSE);
}

// アウトプットペインの現在の表示状態を設定へ取り込む（Issue #148）。
//   原も設定値（CMainFrame +0xb58）を可視状態のミラーとして持ち、トグル（FUN_0042730c）・
//   BGREP（Bgrep_Run）・コマンドUI更新（FUN_0042734d）・終了（FUN_00426e4c）で更新する。
//   移植版でも同じ箇所で同期する。終了経路が OnClose を通らない場合（ログオフ／
//   シャットダウンでは CFrameWnd::OnEndSession が ExitInstance を直接呼ぶ）や、
//   環境設定の OK が設定一式を保存する場合にも、保存される値を最新に保つため。
void CMainFrame::SyncOutputPaneSetting() {
    if (m_wndOutputBar.GetSafeHwnd() == nullptr) { return; }
    theApp.AppSettings().showOutputPane = (m_wndOutputBar.IsWindowVisible() != FALSE);
}

// アウトプットペインの表示/非表示トグル（原 0x80e8）。
void CMainFrame::OnOutputPaneToggle() {
    const BOOL show = !m_wndOutputBar.IsWindowVisible();
    ShowControlBar(&m_wndOutputBar, show, FALSE);
    SyncOutputPaneSetting();
}

void CMainFrame::OnUpdateOutputPaneToggle(CCmdUI* pCmdUI) {
    // 原 FUN_0042734d: アウトプットバーの可視状態でチェックマークを付け、設定へも取り込む。
    //   バー自身の閉じるボタンなど、トグル以外で表示が変わった場合はここで拾う。
    SyncOutputPaneSetting();
    pCmdUI->Enable(TRUE);
    pCmdUI->SetCheck(m_wndOutputBar.GetSafeHwnd() != nullptr &&
                     m_wndOutputBar.IsWindowVisible() ? 1 : 0);
}

void CMainFrame::OnOutputTagJump() { m_wndOutputBar.TagJump(); }
void CMainFrame::OnOutputCopy()    { m_wndOutputBar.CopyToClipboard(); }
void CMainFrame::OnOutputClear()   { m_wndOutputBar.ClearResults(); }

void CMainFrame::OnUpdateOutputHasResults(CCmdUI* pCmdUI) {
    pCmdUI->Enable(m_wndOutputBar.GetSafeHwnd() != nullptr &&
                   m_wndOutputBar.HasResults() ? TRUE : FALSE);
}

// BGREP（0x8039, 原 FUN_00426955）: 設定ダイアログ→アウトプット表示・クリア→
//   検索状況ダイアログ（内部でワーカスレッド走査）。結果はアウトプットペインに集約。
void CMainFrame::OnBgrep() {
    CBgrepDlg dlg(&m_bgrepSettings, this);
    if (dlg.DoModal() != IDOK) {
        return;
    }
    // アウトプットペインを表示＋前回結果をクリア（原 FUN_0042cbd6＋バー表示）。
    ShowControlBar(&m_wndOutputBar, TRUE, FALSE);
    SyncOutputPaneSetting();          // 原 Bgrep_Run も設定値を 1 にする
    m_wndOutputBar.FitList();         // 表示化後にバー全幅へリストを合わせる
    m_wndOutputBar.ClearResults();

    // 検索状況ダイアログをモーダル実行（完了まで制御を返さない＝原の挙動）。
    CBgrepStatusDlg status(dlg.Pattern(), m_bgrepSettings, &m_wndOutputBar, this);
    status.DoModal();
}

// 構造体編集ビューの表示/非表示トグル（原 FUN_004268c1）。表示化時は即時 Refresh。
void CMainFrame::OnStructBarToggle() {
    const BOOL show = !m_wndStructBar.IsWindowVisible();
    ShowControlBar(&m_wndStructBar, show, FALSE);
    if (show) {
        m_wndStructBar.SyncToCaret();   // 表示化時に base←キャレット（原 FUN_00428720）
    } else {
        m_wndStructBar.ClearViewHighlight();   // 非表示化時にデータビューの青装飾を解除
    }
}

// 「キャレット位置を構造体編集」（原 0x8061）: 構造体編集の先頭アドレスをキャレット位置へ。
void CMainFrame::OnStructBarCaret() {
    m_wndStructBar.SetBaseToCaret();
}

void CMainFrame::OnRunApp() {
    RunAppCommand(this);
}

void CMainFrame::OnUpdateStructBarToggle(CCmdUI* pCmdUI) {
    // 原 FUN_00428343: ビュー可視状態でチェックマーク。
    pCmdUI->Enable(TRUE);
    pCmdUI->SetCheck(m_wndStructBar.GetSafeHwnd() != nullptr &&
                     m_wndStructBar.IsWindowVisible() ? 1 : 0);
}

// 現在アクティブな Stirling 文書（無ければ nullptr）。
CStirlingDoc* CMainFrame::ActiveStirlingDoc() {
    if (CMDIChildWnd* pChild = MDIGetActive()) {
        return DYNAMIC_DOWNCAST(CStirlingDoc, pChild->GetActiveDocument());
    }
    return nullptr;
}

// ビットイメージ・ペインの表示/非表示トグル（原 0x80eb FUN_0042827b）。
//   表示化時は現アクティブ文書からイメージを再構築する。
void CMainFrame::OnBitImageToggle() {
    const BOOL show = !m_wndBitImageBar.IsWindowVisible();
    ShowControlBar(&m_wndBitImageBar, show, FALSE);
    if (show) {
        m_wndBitImageBar.Refresh(ActiveStirlingDoc());
    }
    SyncBitImageSetting();
}

void CMainFrame::OnUpdateBitImageToggle(CCmdUI* pCmdUI) {
    // バー自身の閉じるボタンやドラッグでの再配置など、トグル以外で状態が変わった
    //   場合はここで拾う（アウトプットペインと同じ扱い。Issue #121）。
    SyncBitImageSetting();
    // 原 FUN_004282d2: 可視状態でチェックマーク。
    pCmdUI->Enable(TRUE);
    pCmdUI->SetCheck(m_wndBitImageBar.GetSafeHwnd() != nullptr &&
                     m_wndBitImageBar.IsWindowVisible() ? 1 : 0);
}

// 最新イメージを表示（原 0x80ec FUN_0042861e）: 表示中なら現アクティブ文書から再構築。
void CMainFrame::OnBitImageLatest() {
    RefreshBitImage();
}

// 文書データの変更通知（CStirlingDoc::SetModifiedFlag から）。表示中かつ環境設定
//   realtimeBitImage が有効なときだけ、再構築メッセージを1つだけ投函する。
//   ループ内の連続編集（全置換等）でも、処理が message pump へ戻った時点で1回に集約される。
void CMainFrame::QueueBitImageRefresh() {
    if (GetSafeHwnd() == nullptr) { return; }
    if (m_bitImageRefreshPending) { return; }
    if (!theApp.AppSettings().realtimeBitImage) { return; }
    if (m_wndBitImageBar.GetSafeHwnd() == nullptr || !m_wndBitImageBar.IsWindowVisible()) { return; }
    m_bitImageRefreshPending = true;
    PostMessage(kBitImageRefreshMsg);
}

LRESULT CMainFrame::OnBitImageRefreshQueued(WPARAM /*wParam*/, LPARAM /*lParam*/) {
    m_bitImageRefreshPending = false;
    RefreshBitImage();
    return 0;
}

// ビットイメージが表示中ならアクティブ文書から再構築（realtimeBitImage の即時反映で使用）。
void CMainFrame::RefreshBitImage() {
    if (m_wndBitImageBar.GetSafeHwnd() != nullptr && m_wndBitImageBar.IsWindowVisible()) {
        m_wndBitImageBar.Refresh(ActiveStirlingDoc());
    }
}

// 環境設定（原 0x8050 → CPropertySheet に 8 ページ）。
//   OK でアプリ全体設定を確定・設定ストアへ保存し、ツールバー／各バー／文書タイトルへ反映する。
void CMainFrame::OnSettingsEnv() {
    CEnvSheet sheet(theApp.AppSettings(), this);
    if (sheet.DoModal() != IDOK) {
        return;
    }
    theApp.AppSettings() = sheet.m_s;   // 作業コピーを確定
    // ダイアログに項目が無い実行時の状態は、ダイアログを開いた時点の複製で巻き戻るため、
    //   保存の前に現在値へ戻す（Issue #148 / #121）。
    SyncOutputPaneSetting();
    SyncBitImageSetting();
    theApp.AppSettings().Save();        // 設定ストアへ保存
    theApp.ApplyFileHistoryCount();     // ファイル履歴（MRU）数の変更を反映
    ApplyBarSettings();                 // ツールバー構成・バー表示状態を反映
    // 全文書へ反映するもの:
    //   - ドキュメントのフルパス表示（docFullPath）をタイトルへ
    //   - アンドゥバッファのメモリ上限（Issue #102）。次の編集まで待つと、上限を
    //     下げて編集をやめた場合に超過分を抱えたままになるため、その場で切り詰める
    if (POSITION posT = theApp.GetFirstDocTemplatePosition()) {
        CDocTemplate* pTpl = theApp.GetNextDocTemplate(posT);
        if (pTpl != nullptr) {
            POSITION posD = pTpl->GetFirstDocPosition();
            while (posD != nullptr) {
                if (CStirlingDoc* d = DYNAMIC_DOWNCAST(CStirlingDoc, pTpl->GetNextDoc(posD))) {
                    d->RefreshDocTitle();
                    d->ApplyUndoMemoryLimit();
                }
            }
        }
    }
}

// 拡張子別設定（原 0x8051 → 一覧185→設定→拡張子/表示状態158/色フォント107）。
//   一覧ダイアログでレコードを編集し、閉じたら theApp レコードへ反映→保存→全ビュー再適用
//   （原 FUN_00427965: 閉じたら 0x7fff 相当で全ビューが拡張子設定を再解決）。
void CMainFrame::OnSettingsExt() {
    CExtListDlg dlg(this);
    if (dlg.DoModal() != IDOK) {   // 閉じる(IDOK)で確定。X(キャンセル)は破棄
        return;
    }
    theApp.ExtRecords() = dlg.m_records;   // 作業コピーを反映
    theApp.SaveSettings();                 // 設定ストアへ保存

    // 各文書が拡張子レコードを再解決し、全ビューへ反映（原 FUN_00427965 の 0x7fff 相当）。
    std::vector<CStirlingView*> views;
    CStirlingView::EnumAllViews(views);
    for (CStirlingView* v : views) {
        if (v != nullptr) {
            if (CStirlingDoc* d = v->GetDocument()) { d->ResolveSettings(); }
            v->ReloadSettings();
        }
    }
    // ビットイメージ表示中なら配色（反映ON/OFF・強調コード）変更を反映して再構築。
    if (m_wndBitImageBar.GetSafeHwnd() != nullptr && m_wndBitImageBar.IsWindowVisible()) {
        m_wndBitImageBar.Refresh(ActiveStirlingDoc());
    }
}
