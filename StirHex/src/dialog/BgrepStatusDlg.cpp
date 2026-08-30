// CBgrepStatusDlg 実装（BGREP 検索状況＋ワーカスレッド）。
#include "pch.h"
#include "app/UiStrings.h"   // UI文字列はリソースから
#include "dialog/BgrepStatusDlg.h"
#include "frame/OutputBar.h"
#include "core/BlockList.h"
#include "core/BlockCursor.h"
#include "core/BlockFileIO.h"
#include "util/ScopedHandle.h"

#include <vector>

BEGIN_MESSAGE_MAP(CBgrepStatusDlg, CDialog)
    ON_MESSAGE(WM_BGREP_SCAN, &CBgrepStatusDlg::OnScan)
    ON_MESSAGE(WM_BGREP_HIT, &CBgrepStatusDlg::OnHit)
    ON_MESSAGE(WM_BGREP_DONE, &CBgrepStatusDlg::OnDone)
END_MESSAGE_MAP()

// ---------------------------------------------------------------------------
// ワーカスレッド（原 FUN_00402d00→FUN_00402d43→FUN_00402f55/f02）。
//   プロセスのカレントディレクトリは変更せず、絶対パスを直接組み立てる
//   （原は SetCurrentDirectory を使うが、スレッド安全のため本移植では避ける）。
// ---------------------------------------------------------------------------
namespace {

CStringW JoinPath(const CStringW& dir, const wchar_t* name) {
    CStringW p = dir;
    if (!p.IsEmpty()) {
        const wchar_t last = p[p.GetLength() - 1];
        if (last != L'\\' && last != L'/') { p += L"\\"; }
    }
    p += name;
    return p;
}

// 1 ファイルを検索（原 FUN_00402f55）。走査通知→BlockList ロード→Horspool 反復。
void SearchOneFile(BgrepJob* job, const CStringW& full) {
    stirling::BlockList bl;
    // core はワイドパス・結果型の API（Issue #20）。パスは wide 層のまま渡す。
    const bool ok = stirling::LoadFileIntoBlocks(bl, full).Ok();
    // 総長は 64bit で保持する。int に丸めると 2GB 超で負値となり、
    // 下の走査ループが初回で打ち切られる（Issue #19）。
    const stirling::FileOffset total = ok ? stirling::RecalcTotalLength(bl) : 0;

    // 走査通知（サイズ 0＝開けない/空 は受信側で「アクセス拒否」行を追記）。
    // Issue #156: サイズは 64bit のまま構造体で渡す（WPARAM は Win32 で 32bit）。
    //   SendMessageW は同期のため、スタック上の構造体を指したままで安全。
    stirling::BgrepScanNotify scan;
    scan.path = full.GetString();
    scan.size = total;
    ::SendMessageW(job->notify, WM_BGREP_SCAN, 0, (LPARAM)&scan);
    if (!ok || total == 0) { return; }

    const int plen = (int)job->pattern.size();
    if (plen == 0 || total < plen) { return; }

    stirling::BlockCursor cur(&bl);
    stirling::FileOffset pos = 0;         // core は 64bit 位置（Issue #19）
    stirling::FileOffset hit = 0;
    while (job->running != 0 &&
           cur.SearchPattern(job->pattern.data(), plen, &hit,
                             stirling::BlockCursor::kForward, pos, 0)) {
        // Issue #156: ヒット位置も 64bit のまま構造体で渡す（同上）。
        stirling::BgrepHitNotify notify;
        notify.path = full.GetString();
        notify.pos = hit;
        ::SendMessageW(job->notify, WM_BGREP_HIT, 0, (LPARAM)&notify);
        pos = hit + plen;
        if (pos > total - plen) { break; }
    }
}

// 1 ディレクトリを走査（原 FUN_00402d43）。マスク毎にファイル検索し、再帰時は
//   サブフォルダへ降りる（原 FUN_00402f02）。
void WalkDirectory(BgrepJob* job, const CStringW& dir) {
    if (job->running == 0) { return; }

    // マスク毎のファイル走査。
    for (size_t i = 0; i < job->masks.size() && job->running != 0; ++i) {
        const CStringW pattern = JoinPath(dir, job->masks[i]);
        WIN32_FIND_DATAW fd;
        stirling::ScopedFindHandle find(::FindFirstFileW(pattern, &fd));
        if (!find.Valid()) { continue; }
        do {
            if (job->running == 0) { break; }
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { continue; }
            if (fd.cFileName[0] == L'.') { continue; }
            if (job->skipSystem && (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)) { continue; }
            SearchOneFile(job, JoinPath(dir, fd.cFileName));
        } while (::FindNextFileW(find.Get(), &fd));
    }

    // サブフォルダ再帰。
    if (job->recurse && job->running != 0) {
        const CStringW pattern = JoinPath(dir, L"*");
        WIN32_FIND_DATAW fd;
        stirling::ScopedFindHandle find(::FindFirstFileW(pattern, &fd));
        if (find.Valid()) {
            do {
                if (job->running == 0) { break; }
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { continue; }
                if (fd.cFileName[0] == L'.') { continue; }
                if (job->skipSystem && (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)) { continue; }
                WalkDirectory(job, JoinPath(dir, fd.cFileName));
            } while (::FindNextFileW(find.Get(), &fd));
        }
    }
}

UINT AFX_CDECL BgrepWorker(LPVOID param) {
    BgrepJob* job = static_cast<BgrepJob*>(param);
    WalkDirectory(job, job->root);
    ::PostMessage(job->notify, WM_BGREP_DONE, 0, 0);   // 完了（以降 job には触れない）
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------

CBgrepStatusDlg::CBgrepStatusDlg(const std::vector<unsigned char>& pattern,
                                 const BgrepSettings& s,
                                 CStirlingOutputBar* outbar, CWnd* pParent)
    : CDialog(IDD_BGREP_STATUS, pParent)
    , m_outbar(outbar)
    , m_hitCount(0)
    , m_fileCount(0) {
    m_job.pattern = pattern;
    m_job.root = s.folder;
    m_job.recurse = s.recurse;
    m_job.skipSystem = s.skipSystem;
    m_job.running = 1;

    // ファイル種類（";" 区切り）をマスク配列へ分解。空要素は無視。
    CStringW masks = s.fileMask;
    int start = 0;
    while (start <= masks.GetLength()) {
        int sep = masks.Find(L';', start);
        if (sep < 0) { sep = masks.GetLength(); }
        CStringW one = masks.Mid(start, sep - start);
        one.Trim(L" \t");
        if (!one.IsEmpty()) { m_job.masks.push_back(one); }
        start = sep + 1;
    }
    if (m_job.masks.empty()) { m_job.masks.push_back(L"*.*"); }
}

BOOL CBgrepStatusDlg::OnInitDialog() {
    CDialog::OnInitDialog();
    SetDlgItemText(IDC_BGREP_STAT_FILE, _T(""));
    SetDlgItemInt(IDC_BGREP_STAT_COUNT, 0);
    m_job.notify = GetSafeHwnd();
    // ワーカスレッド起動（CWinThread は既定で自動破棄）。
    AfxBeginThread(BgrepWorker, &m_job);
    return TRUE;
}

// キャンセル: 停止フラグを落とすのみ。ワーカの WM_BGREP_DONE で閉じる。
void CBgrepStatusDlg::OnCancel() {
    ::InterlockedExchange(&m_job.running, 0);
    if (CWnd* pBtn = GetDlgItem(IDCANCEL)) { pBtn->EnableWindow(FALSE); }
}

// 走査通知（原 0x414）。サイズ 0 は開けない/空＝アウトプットへアクセス拒否行。
//   lParam は送信側スタック上の BgrepScanNotify（Issue #156）。
LRESULT CBgrepStatusDlg::OnScan(WPARAM /*wParam*/, LPARAM lParam) {
    ++m_fileCount;
    const stirling::BgrepScanNotify* n = (const stirling::BgrepScanNotify*)lParam;
    if (n == nullptr) { return 0; }
    const CStringW path(n->path);
    if (n->size == 0) {
        if (m_outbar != nullptr) {
            m_outbar->AddMessage(path + ui::LoadW(IDS_BGREP_ACCESS_DENIED));
        }
    } else {
        SetDlgItemText(IDC_BGREP_STAT_FILE, path);
    }
    return 0;
}

// ヒット通知（原 0x415）。件数更新＋アウトプットペインへ 1 行追記。
//   lParam は送信側スタック上の BgrepHitNotify（Issue #156）。
LRESULT CBgrepStatusDlg::OnHit(WPARAM /*wParam*/, LPARAM lParam) {
    const stirling::BgrepHitNotify* n = (const stirling::BgrepHitNotify*)lParam;
    if (n == nullptr) { return 0; }
    ++m_hitCount;
    SetDlgItemInt(IDC_BGREP_STAT_COUNT, m_hitCount);
    if (m_outbar != nullptr) {
        m_outbar->AddResult(CStringW(n->path), n->pos);   // 位置は 64bit のまま
    }
    return 0;
}

// 完了通知（原 0x417）。集計行を追記してダイアログを閉じる。
LRESULT CBgrepStatusDlg::OnDone(WPARAM /*wParam*/, LPARAM /*lParam*/) {
    if (m_outbar != nullptr) {
        if (m_fileCount == 0) {
            m_outbar->AddMessage(ui::LoadW(IDS_BGREP_NO_FILES));
        } else if (m_hitCount == 0) {
            m_outbar->AddMessage(ui::LoadW(IDS_SEARCH_NOTFOUND));   // 原 1006
        } else {
            CStringW w;
            w.Format(ui::LoadW(IDS_BGREP_HIT_COUNT), m_hitCount);
            m_outbar->AddMessage(w);
        }
    }
    EndDialog(IDOK);
    return 0;
}
