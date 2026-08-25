// CAppSettings 実装。theApp（CWinApp）のプロファイルAPI経由でレジストリと往復する。
#include "pch.h"
#include "app/AppSettings.h"

namespace {
const TCHAR* kSec = _T("Env");

int  GetInt(const TCHAR* key, int def)        { return AfxGetApp()->GetProfileInt(kSec, key, def); }
void PutInt(const TCHAR* key, int val)        { AfxGetApp()->WriteProfileInt(kSec, key, val); }
bool GetBool(const TCHAR* key, bool def)      { return GetInt(key, def ? 1 : 0) != 0; }
void PutBool(const TCHAR* key, bool val)      { PutInt(key, val ? 1 : 0); }

// 文字列設定（フォルダ等。wide 層。Unicode ビルドではプロファイル API がワイド）。
CStringW GetStr(const TCHAR* key) {
    return AfxGetApp()->GetProfileString(kSec, key, _T(""));
}
void PutStr(const TCHAR* key, const CStringW& val) {
    AfxGetApp()->WriteProfileString(kSec, key, val);
}
}

// 既定キーマップ（原 FUN_0041972b を忠実再現）。index = modstate*0x40 + keycode, 値 = 機能rawID。
std::vector<UINT> CAppSettings::BuildDefaultKeymap() {
    std::vector<UINT> m(kKeymapSize, 0);
    auto set = [&](int i, UINT v) { if (i >= 0 && i < kKeymapSize) { m[i] = v; } };
    set(0x00, 0x709); set(0x02, 0x408); set(0x2e, 0x106); set(0x2f, 0x107); set(0x36, 0x306);
    set(0x42, 0x409); set(0x6e, 0x207); set(0x6f, 0x208); set(0x76, 0x304); set(0x77, 0x309); set(0x78, 0x309);
    set(0x70, 0x201); set(0x71, 0x202); set(0x72, 0x203); set(0x73, 0x204); set(0x74, 0x20a); set(0x75, 0x20b);
    set(0x83, 0x003); set(0x85, 0x605); set(0xae, 0x104); set(0xaf, 0x105); set(0xb6, 0x303);
    set(0xb0, 0x106); set(0xb1, 0x107); set(0x8f, 0x303); set(0x92, 0x400); set(0xa2, 0x304);
    set(0xa4, 0x302); set(0xa6, 0x300); set(0xee, 0x205); set(0xef, 0x206); set(0xe6, 0x301);
    return m;
}

// 原の既定ユーザーメニュー。メニュー1-10/2ストローク/Escは空、コンテキストメニュー(idx14)のみ
//   原の初期構成（切り取り/コピー/貼り付け/区切り/選択範囲をファイルに保存/削除/初期化）。
std::vector<std::vector<UINT>> CAppSettings::BuildDefaultUserMenus() {
    std::vector<std::vector<UINT>> v(kUserMenuCount);
    // 原の既定コンテキストメニュー（FUN_0041f2a5 のフォールバック 0x540302 等）。
    //   各アイテム = (アクセラレータ<<16)|rawID。アクセラレータは標準編集メニューのニーモニック。
    v[kContextMenuIndex] = {
        UmMake('T', 0x0302),   // 切り取り（&T）
        UmMake('C', 0x0303),   // コピー（&C）
        UmMake('P', 0x0304),   // 貼り付け（&P）
        kUserMenuSep,          // セパレータ
        UmMake('S', 0x030C),   // 選択範囲をファイルに保存...（&S）
        UmMake('D', 0x030A),   // 選択範囲の削除（&D）
        UmMake('I', 0x030B),   // 選択範囲の初期化...（&I）
    };
    return v;
}

void CAppSettings::Load() {
    scrollLines       = GetInt(_T("ScrollLines"), scrollLines);
    pasteOverwrite    = GetBool(_T("PasteOverwrite"), pasteOverwrite);
    searchNotFoundMsg = GetBool(_T("SearchNotFoundMsg"), searchNotFoundMsg);
    escMenu           = GetBool(_T("EscMenu"), escMenu);
    escDeselect       = GetBool(_T("EscDeselect"), escDeselect);
    deselectAfterCopy = GetBool(_T("DeselectAfterCopy"), deselectAfterCopy);
    clearUndoOnSave   = GetBool(_T("ClearUndoOnSave"), clearUndoOnSave);
    undoMemoryLimitMB = GetInt(_T("UndoMemoryLimitMB"), undoMemoryLimitMB);
    subCaret          = GetBool(_T("SubCaret"), subCaret);
    highlightBoth     = GetBool(_T("HighlightBoth"), highlightBoth);
    realtimeBitImage  = GetBool(_T("RealtimeBitImage"), realtimeBitImage);
    twoStrokeTimeoutMs = GetInt(_T("TwoStrokeTimeoutMs"), twoStrokeTimeoutMs);

    fileHistoryCount   = GetInt(_T("FileHistoryCount"), fileHistoryCount);
    caretAutoRestore   = GetBool(_T("CaretAutoRestore"), caretAutoRestore);
    curPosToStructAddr = GetBool(_T("CurPosToStructAddr"), curPosToStructAddr);
    newDocEditable     = GetBool(_T("NewDocEditable"), newDocEditable);
    endAutoInsert      = GetBool(_T("EndAutoInsert"), endAutoInsert);
    dynamicMark        = GetBool(_T("DynamicMark"), dynamicMark);

    backupCreate         = GetBool(_T("BackupCreate"), backupCreate);
    backupGenerations    = GetInt(_T("BackupGenerations"), backupGenerations);
    backupFolderSpecify  = GetBool(_T("BackupFolderSpecify"), backupFolderSpecify);
    backupFolder         = GetStr(_T("BackupFolder"));
    exclusiveControl     = GetInt(_T("ExclusiveControl"), exclusiveControl);
    linkDirect           = GetBool(_T("LinkDirect"), linkDirect);
    defaultFolderSpecify = GetBool(_T("DefaultFolderSpecify"), defaultFolderSpecify);
    defaultFolder        = GetStr(_T("DefaultFolder"));

    winPlacement    = GetInt(_T("WinPlacement"), winPlacement);
    winLeft         = GetInt(_T("WinLeft"), winLeft);
    winTop          = GetInt(_T("WinTop"), winTop);
    winWidth        = GetInt(_T("WinWidth"), winWidth);
    winHeight       = GetInt(_T("WinHeight"), winHeight);
    docMaximize     = GetBool(_T("DocMaximize"), docMaximize);
    docFullPath     = GetBool(_T("DocFullPath"), docFullPath);
    showToolbar     = GetBool(_T("ShowToolbar"), showToolbar);
    showStatusbar   = GetBool(_T("ShowStatusbar"), showStatusbar);
    allowMultipleInstances = GetBool(_T("AllowMultipleInstances"), allowMultipleInstances);
    bitImageDockable = GetBool(_T("BitImageDockable"), bitImageDockable);
    structBarPos       = GetInt(_T("StructBarPos"), structBarPos);
    structBarNoDock    = GetBool(_T("StructBarNoDock"), structBarNoDock);
    structBarStatusPos = GetInt(_T("StructBarStatusPos"), structBarStatusPos);
    structItemRatioKeep = GetBool(_T("StructItemRatioKeep"), structItemRatioKeep);

    // ステータスバー構成（保存があれば復元、無ければ既定を維持）。
    {
        const int n = GetInt(_T("StatusItemCount"), -1);
        if (n >= 0) {
            statusItems.clear();
            for (int i = 0; i < n; ++i) {
                CString key; key.Format(_T("StatusItem%d"), i);
                statusItems.push_back((UINT)GetInt(key, 0));
            }
        }
    }
    // ツールバー構成（保存があれば復元、無ければ既定を維持）。
    {
        const int n = GetInt(_T("ToolbarItemCount"), -1);
        if (n >= 0) {
            toolbarItems.clear();
            for (int i = 0; i < n; ++i) {
                CString key; key.Format(_T("ToolbarItem%d"), i);
                toolbarItems.push_back((UINT)GetInt(key, 0xFFFF));
            }
        }
    }
    // キーマップ（保存があればバイナリ復元、無ければ既定を維持）。
    {
        LPBYTE blob = nullptr; UINT bytes = 0;
        if (AfxGetApp()->GetProfileBinary(kSec, _T("Keymap"), &blob, &bytes) &&
            blob != nullptr && bytes == kKeymapSize * sizeof(UINT)) {
            keymap.assign((UINT*)blob, (UINT*)blob + kKeymapSize);
        }
        delete[] blob;
    }
    // ユーザーメニュー構成（保存があれば各メニューを復元）。
    {
        const int mc = GetInt(_T("UserMenuCount"), -1);
        if (mc == kUserMenuCount) {
            for (int m = 0; m < kUserMenuCount; ++m) {
                CString ck; ck.Format(_T("UserMenu%d_Count"), m);
                const int n = GetInt(ck, 0);
                userMenus[m].clear();
                for (int i = 0; i < n; ++i) {
                    CString key; key.Format(_T("UserMenu%d_%d"), m, i);
                    userMenus[m].push_back((UINT)GetInt(key, 0xFFFF));
                }
            }
        }
    }

    // 範囲補正。
    if (scrollLines < 1) { scrollLines = 1; }
    if (twoStrokeTimeoutMs < 0) { twoStrokeTimeoutMs = 0; }
    if (fileHistoryCount < 0)  { fileHistoryCount = 0; }
    if (fileHistoryCount > 16) { fileHistoryCount = 16; }
    if (backupGenerations < 1)   { backupGenerations = 1; }
    if (backupGenerations > 999) { backupGenerations = 999; }
    if (exclusiveControl < 0 || exclusiveControl > 2) { exclusiveControl = 0; }
    if (winPlacement < 0 || winPlacement > 3) { winPlacement = 1; }
    if (structBarPos < 0 || structBarPos > 2) { structBarPos = 0; }
    if (structBarStatusPos < 0 || structBarStatusPos > 2) { structBarStatusPos = 2; }
}

void CAppSettings::Save() const {
    PutInt(_T("ScrollLines"), scrollLines);
    PutBool(_T("PasteOverwrite"), pasteOverwrite);
    PutBool(_T("SearchNotFoundMsg"), searchNotFoundMsg);
    PutBool(_T("EscMenu"), escMenu);
    PutBool(_T("EscDeselect"), escDeselect);
    PutBool(_T("DeselectAfterCopy"), deselectAfterCopy);
    PutBool(_T("ClearUndoOnSave"), clearUndoOnSave);
    PutInt(_T("UndoMemoryLimitMB"), undoMemoryLimitMB);
    PutBool(_T("SubCaret"), subCaret);
    PutBool(_T("HighlightBoth"), highlightBoth);
    PutBool(_T("RealtimeBitImage"), realtimeBitImage);
    PutInt(_T("TwoStrokeTimeoutMs"), twoStrokeTimeoutMs);

    PutInt(_T("FileHistoryCount"), fileHistoryCount);
    PutBool(_T("CaretAutoRestore"), caretAutoRestore);
    PutBool(_T("CurPosToStructAddr"), curPosToStructAddr);
    PutBool(_T("NewDocEditable"), newDocEditable);
    PutBool(_T("EndAutoInsert"), endAutoInsert);
    PutBool(_T("DynamicMark"), dynamicMark);

    PutBool(_T("BackupCreate"), backupCreate);
    PutInt(_T("BackupGenerations"), backupGenerations);
    PutBool(_T("BackupFolderSpecify"), backupFolderSpecify);
    PutStr(_T("BackupFolder"), backupFolder);
    PutInt(_T("ExclusiveControl"), exclusiveControl);
    PutBool(_T("LinkDirect"), linkDirect);
    PutBool(_T("DefaultFolderSpecify"), defaultFolderSpecify);
    PutStr(_T("DefaultFolder"), defaultFolder);

    PutInt(_T("WinPlacement"), winPlacement);
    PutInt(_T("WinLeft"), winLeft);
    PutInt(_T("WinTop"), winTop);
    PutInt(_T("WinWidth"), winWidth);
    PutInt(_T("WinHeight"), winHeight);
    PutBool(_T("DocMaximize"), docMaximize);
    PutBool(_T("DocFullPath"), docFullPath);
    PutBool(_T("ShowToolbar"), showToolbar);
    PutBool(_T("ShowStatusbar"), showStatusbar);
    PutBool(_T("AllowMultipleInstances"), allowMultipleInstances);
    PutBool(_T("BitImageDockable"), bitImageDockable);
    PutInt(_T("StructBarPos"), structBarPos);
    PutBool(_T("StructBarNoDock"), structBarNoDock);
    PutInt(_T("StructBarStatusPos"), structBarStatusPos);
    PutBool(_T("StructItemRatioKeep"), structItemRatioKeep);

    PutInt(_T("StatusItemCount"), (int)statusItems.size());
    for (int i = 0; i < (int)statusItems.size(); ++i) {
        CString key; key.Format(_T("StatusItem%d"), i);
        PutInt(key, (int)statusItems[i]);
    }
    PutInt(_T("ToolbarItemCount"), (int)toolbarItems.size());
    for (int i = 0; i < (int)toolbarItems.size(); ++i) {
        CString key; key.Format(_T("ToolbarItem%d"), i);
        PutInt(key, (int)toolbarItems[i]);
    }
    if ((int)keymap.size() == kKeymapSize) {
        AfxGetApp()->WriteProfileBinary(kSec, _T("Keymap"),
            (LPBYTE)keymap.data(), kKeymapSize * sizeof(UINT));
    }
    PutInt(_T("UserMenuCount"), kUserMenuCount);
    for (int m = 0; m < (int)userMenus.size() && m < kUserMenuCount; ++m) {
        CString ck; ck.Format(_T("UserMenu%d_Count"), m);
        PutInt(ck, (int)userMenus[m].size());
        for (int i = 0; i < (int)userMenus[m].size(); ++i) {
            CString key; key.Format(_T("UserMenu%d_%d"), m, i);
            PutInt(key, (int)userMenus[m][i]);
        }
    }
}
