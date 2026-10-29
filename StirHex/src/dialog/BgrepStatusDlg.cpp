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
    ON_MESSAGE(WM_BGREP_READFAIL, &CBgrepStatusDlg::OnReadFail)
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
    //   検索は読むだけなので、ファイル全体を読み込まず必要な部分だけ読む（Issue #300）。
    //   共有は従来の全体読み込みと同じく全許可（他プロセスの編集を妨げない）。
    const bool ok =
        stirling::LoadFileLazy(bl, full, stirling::FileShareMode::kDenyNone).Ok();
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

    // 一致の開始位置を kWindow バイトずつの区間に分けて検索し、区間ごとに中止を確認する。
    //   ファイル末尾まで一度に検索すると、一致しない巨大ファイルでは読み終えるまで
    //   中止できなかった（Issue #306）。区間の終端は、区間内で始まる一致が収まるよう
    //   パターン長 - 1 だけ先まで含める（SearchPattern の end は一致の末尾の上限）。
    const stirling::FileOffset kWindow = 16 * 1024 * 1024;
    stirling::BlockCursor cur(&bl);
    stirling::FileOffset pos = 0;         // core は 64bit 位置（Issue #19）
    while (job->running != 0 && pos <= total - plen) {
        const stirling::FileOffset end =
            (total - pos > kWindow + plen - 1) ? pos + kWindow + plen - 1 : total;
        stirling::FileOffset hit = 0;
        if (!cur.SearchPattern(job->pattern.data(), plen, &hit,
                               stirling::BlockCursor::kForward, pos, end)) {
            // 読めずに打ち切った場合は「見つからない」と区別して知らせ、このファイルの
            //   残りは検索しない（検索中に他プロセスが切り詰めた・範囲をロックした等。
            //   Issue #324）。それまでのヒットは通知済み。
            if (cur.ReadFailed()) {
                stirling::BgrepReadFailNotify failed;
                failed.path = full.GetString();
                ::SendMessageW(job->notify, WM_BGREP_READFAIL, 0, (LPARAM)&failed);
                return;
            }
            if (end == total) { break; }  // 末尾まで検索し終えた
            pos = end - (plen - 1);       // 次の区間（= pos + kWindow）
            continue;
        }
        // Issue #156: ヒット位置も 64bit のまま構造体で渡す（同上）。
        stirling::BgrepHitNotify notify;
        notify.path = full.GetString();
        notify.pos = hit;
        ::SendMessageW(job->notify, WM_BGREP_HIT, 0, (LPARAM)&notify);
        pos = hit + plen;
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
    if (n == nullptr || m_truncated) { return 0; }
    // 上限を超えるヒットは追加せず、ワーカに停止を求める（Issue #307）。ヒットが数十億件に
    //   なる検索（巨大ファイルで 00 を探す等）でも、出力ペインとメモリが際限なく増えない。
    //   ワーカは SendMessage から戻った後のループで停止に気付く。それまでの通知は捨てる。
    if (m_hitCount >= kHitLimit) {
        m_truncated = true;
        ::InterlockedExchange(&m_job.running, 0);
        return 0;
    }
    ++m_hitCount;
    SetDlgItemInt(IDC_BGREP_STAT_COUNT, m_hitCount);
    if (m_outbar != nullptr) {
        m_outbar->AddResult(CStringW(n->path), n->pos);   // 位置は 64bit のまま
    }
    return 0;
}

// 読取失敗通知（移植版で追加。Issue #324）。アクセス拒否と同じ形の行を追記する。
//   lParam は送信側スタック上の BgrepReadFailNotify。
LRESULT CBgrepStatusDlg::OnReadFail(WPARAM /*wParam*/, LPARAM lParam) {
    const stirling::BgrepReadFailNotify* n = (const stirling::BgrepReadFailNotify*)lParam;
    // 上限で打ち切った後の通知は捨てる（OnHit と同じ。打ち切りの行が最後になるように）。
    if (n == nullptr || m_truncated) { return 0; }
    if (m_outbar != nullptr) {
        m_outbar->AddMessage(CStringW(n->path) + ui::LoadW(IDS_BGREP_READ_FAILED));
    }
    return 0;
}

// 完了通知（原 0x417）。集計行を追記してダイアログを閉じる。
LRESULT CBgrepStatusDlg::OnDone(WPARAM /*wParam*/, LPARAM /*lParam*/) {
    if (m_outbar != nullptr) {
        if (m_fileCount == 0) {
            m_outbar->AddMessage(ui::LoadW(IDS_BGREP_NO_FILES));
        } else if (m_truncated) {
            CStringW w;
            w.Format(ui::LoadW(IDS_BGREP_TRUNCATED), kHitLimit);
            m_outbar->AddMessage(w);
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
