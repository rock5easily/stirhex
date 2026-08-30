// CStirlingView — 16進ビュー（原 CStirlingView : CView）。
// 原は表示設定を CMainFrame 共有オブジェクト(view+0x248)から WM_USER で取得するが、
// 移植では文書が保持する拡張子別設定をビューへ反映する。3カラム描画、縦横スクロール、
// キャレット／選択、文字セット別レンダラ、バイト単位色を実装済み。
#pragma once

#include "core/CoreTypes.h"   // stirling::FileOffset（アドレスの 64bit 化。Issue #21）

#include <vector>
#include <string>
#include <utility>

// 外部プロセスによるファイル変更の確認（原 WM_USER+0x1B = 0x041B）。
//   原はビューのアクティブ化時に自身へポストし、そのハンドラで更新日時を突き合わせる。
#define WM_STIRLING_CHECK_FILE  (WM_USER + 0x1B)

class CStirlingDoc;
class CFindDlg;
class CDiffListDlg;
class CStirlingSettings;   // CurSettings() の参照戻り用（実体は app/StirlingSettings.h）

class CStirlingView : public CView {
    DECLARE_DYNCREATE(CStirlingView)
public:
    CStirlingView();
    virtual ~CStirlingView();

    CStirlingDoc* GetDocument() const;

    virtual void OnDraw(CDC* pDC);
    virtual BOOL PreCreateWindow(CREATESTRUCT& cs);
    virtual void OnUpdate(CView* pSender, LPARAM lHint, CObject* pHint);  // 読込/更新後にスクロール再計算
    virtual void OnInitialUpdate();   // 文書接続後: この文書の拡張子設定をビューへ反映
    // アクティブ化時に外部変更の確認を自身へポストする（原 CView::OnActivateView @0x00450d2c）。
    virtual void OnActivateView(BOOL bActivate, CView* pActivateView, CView* pDeactiveView);

    // --- 印刷（原 CStirlingView 印刷仮想関数4種の移植）---
    //   整形ダンプ（アドレス／16進／文字欄）をプリンタDCへ割付ける。フォントはＭＳ明朝(h100)。
    //   色はモノクロ（黒文字/白地）、比較差分バイトのみ反転（原 FUN_0045d161）。
    virtual BOOL OnPreparePrinting(CPrintInfo* pInfo);                 // 原 0x4427e5
    virtual void OnBeginPrinting(CDC* pDC, CPrintInfo* pInfo);         // 原 0x442840（ページ数算出）
    virtual void OnPrint(CDC* pDC, CPrintInfo* pInfo);                 // 原 0x442b29（1ページ描画）
    virtual void OnEndPrinting(CDC* pDC, CPrintInfo* pInfo);           // 原 0x442af3（後始末）
    virtual void OnEndPrintPreview(CDC* pDC, CPrintInfo* pInfo, POINT point, CPreviewView* pView);
    // 印刷プレビューを子フレーム内ではなく**メインフレーム全体**に表示する（原の挙動）。
    afx_msg void OnFilePrintPreview();

    // 検索ダイアログ（モーダル）の Next/Prev から呼ばれる公開I/F。
    //   検索バイト列で検索を実行（直近条件を記録し、条件変更時は全体検索フラグをリセット）。
    void FindWithBytes(const std::vector<unsigned char>& pattern, int rangeMode, bool forward);
    //   不一致検索（原 0x8032）: 指定1バイトに一致しない最初のバイトを検索（前/次）。
    void FindMismatchWithByte(unsigned char value, int rangeMode, bool forward);
    //   文字列入力→現文字セットのバイト列へ変換（16進の検証/正規化はダイアログ側で行う）。
    std::vector<unsigned char> BuildTextBytes(LPCWSTR text) const;
    // 指定文字セットで文字列→バイト列へ変換（ビュー非依存。BGREP 等が使用）。
    //   原 FUN_004600f6 相当。Unicode は入力時と同じく LE 固定。
    static std::vector<unsigned char> EncodeText(int charset, LPCWSTR text);
    bool HasSelection() const { return m_selActive; }   // 検索ダイアログの「選択範囲内」活性判定

    // --- マーク一覧ダイアログ（CMarkListDlg）の公開I/F ---
    // 現在キャレット位置（新規登録の既定アドレス）
    stirling::FileOffset CurrentPos() const { return m_caretPos; }
    // データ総サイズ（有効アドレス上限＝total-1）
    stirling::FileOffset TotalBytes() const { return Total(); }
    void JumpToMark(stirling::FileOffset pos);   // 指定マーク位置へキャレット移動（実行）
    // 指定位置[0,total]へ移動＋画面外なら縦中央（原mode2）
    void GotoPos(stirling::FileOffset pos);
    // 環境設定の変更を反映する（原 FUN_00427a73→各ビュー再読込）。表示設定(1行バイト数/基数/色/
    //   フォント)を theApp.Settings() から再取得し、メトリクス再計算・スクロール更新・再描画する。
    void ReloadSettings();

    // --- データ比較（原 view+0x2fc 差分配列 / +0x300 強調表示） ---
    //   diffs = 相違範囲[start,end]（両端含む）の昇順リスト。設定で比較色を表示。
    // 相違範囲[start,end]（両端含む）。Issue #21 で 64bit 化。
    void SetCompareResult(const std::vector<std::pair<stirling::FileOffset, stirling::FileOffset>>& diffs);
    void ClearCompareResult();                        // 比較モード解除（差分クリア）
    void SetCompareHighlight(bool on);                // 強調表示ON/OFF（差分は保持。原 view+0x300）
    bool HasCompareDiffs() const { return !m_compareDiffs.empty(); }
    // 相違範囲[start,end]を選択＋縦中央（ジャンプ）
    void GotoCompareDiff(stirling::FileOffset start, stirling::FileOffset end);

    // --- 構造体編集の表示範囲ハイライト（原 view+0x304/0x308/0x30c） ---
    //   構造体編集ビューが表示中の範囲[start,end]（両端含む）を16進欄で青文字にする。
    // 範囲設定＋再描画
    void SetStructHighlight(stirling::FileOffset start, stirling::FileOffset end);
    void ClearStructHighlight();                   // 解除＋再描画

    // --- シンクロスクロール（相違一覧ダイアログ／手動登録ダイアログが複数ビューを連動） ---
    void SetSyncPartner(CStirlingView* partner, bool enabled);   // 2ビュー連動（相違一覧用）
    // 同期グループを確定する（原 OnSyncScroll のグループ再構成）。
    //   members は自分を含む全メンバー。旧グループを解除し、新メンバー全員へ相互設定する。
    void ApplySyncGroup(const std::vector<CStirlingView*>& members);
    const std::vector<CStirlingView*>& SyncGroup() const { return m_syncGroup; }
    // 同期用: 先頭行を直接設定（伝播しない）
    void SetTopLineDirect(stirling::FileOffset top);
    void SyncPropagate();                             // 先頭行を同期グループ全員へ伝播
    stirling::FileOffset TopLine() const { return m_topLine; }
    // 開いている全 CStirlingView を列挙（原 FUN_0044cbad 相当）。
    static void EnumAllViews(std::vector<CStirlingView*>& out);

    // --- 相違一覧ダイアログ（モードレス）の所有 ---
    void SetDiffDlg(CDiffListDlg* dlg) { m_pDiffDlg = dlg; }
    CDiffListDlg* DiffDlg() const { return m_pDiffDlg; }
    void OnDiffDlgClosed() { m_pDiffDlg = nullptr; }

protected:
    // --- 表示設定（原 view+0x248 相当。文書の拡張子別設定をビュー用にキャッシュ） ---
    int m_bytesPerRow;   // +0xa0 相当（1行バイト数）
    int m_addrRadix;     // +0xac 相当（0=10進 / 非0=16進）

    // --- フォント/メトリクス（原 view+0xa8/0xac 相当） ---
    CFont m_font;
    // UTF-8 文字欄用フォント（DEFAULT_CHARSET。GDI のフォントリンクで CP932 外の
    //   グリフを補わせる。桁幅・行高は m_font のものを使うのでレイアウトは変わらない）。
    CFont m_fontUtf8;
    // コードポイント → 表示セル数（0=未測定 / 1 / 2）。フォント変更時に破棄する。
    std::vector<unsigned char> m_utf8CellWidth;
    int   m_charW;       // 文字幅(px)
    int   m_rowH;        // 行高(px)

    // --- スクロール状態（原 view+0x15c 相当：先頭表示行） ---
    // x64 化(Issue #21): 行番号は「総バイト数 / 1行バイト数」。1行2バイトの設定では
    // 4GB 超のファイルで INT_MAX を超えるため FileOffset で保持する。
    stirling::FileOffset m_topLine;
    int m_wheelAccum = 0;   // ホイール端数の累積（高分解能ホイール/タッチパッド対応）
    int m_hScroll = 0;      // 横スクロール位置(px)。原 view の横スクロール相当

    // --- 横スクロール補助 ---
    int  ContentWidthPx() const;   // レイアウト全体の横幅(px, 未スクロール基準)
    int  MaxHScroll() const;       // 横スクロール最大位置(px)
    bool FreezeAddr() const;       // 「アドレスも横スクロール対象」OFF＝アドレス欄固定

    // --- キャレット/入力状態（原 view+0x110..0x16c 相当） ---
    stirling::FileOffset m_caretPos;   // 絶対バイト境界 [0, 総長]（原は列/行。abs=行*bpr+列）
    int  m_activePane;    // +0x11c 相当（0=16進ペイン / 1=文字ペイン）
    bool m_nibbleLow;     // +0x16c 相当（false=上位ニブル待ち / true=下位ニブル待ち）
    bool m_caretShown;    // システムキャレットの表示状態管理
    CRect m_subCaretRect;         // 直近に描いたサブキャレット下線のクライアント矩形
    bool  m_subCaretDrawn = false; // サブキャレット下線を描画中か（無効化用）

    // --- 選択状態（原 view+0x124/0x12c/0x130/0x138/0x13c 相当。ここでは絶対境界で保持） ---
    bool m_selActive;     // 選択有効（アンカー≠キャレット）
    // 選択の固定端（絶対境界）。選択範囲=[min(anchor,caret),max)
    stirling::FileOffset m_selAnchor;
    bool m_dragging;      // +0x120 相当（マウスドラッグ中）
    // DOS式選択モード（原 view+0x124 の状態機械。選択モード開始/終了 0x801c）。
    //   有効時はプレーンなカーソル移動が選択を拡張する（extend=true 相当）。
    bool m_selectMode = false;

    // ２ストロークキー状態（原 view+0xf40）。-1=保留なし / それ以外=待機中の userMenus インデックス
    //   （10-12=2ストローク機能1-3, 13=Escメニュー）。第1打鍵でタイマ開始→第2打鍵でアクセラレータ
    //   照合、タイムアウトで視覚ポップアップ、Escでキャンセル（原 FUN_004273ec/FUN_004275e5/FUN_00427591）。
    int m_twoStrokeMenuIdx = -1;
    static const UINT_PTR kTwoStrokeTimerId = 1;   // 原のタイマID 1
    // ２ストローク第2打鍵など、OnKeyDown で消費したキーが TranslateMessage 経由で生成する
    //   WM_CHAR（文字入力）を1回だけ抑止するフラグ（原は PreTranslateMessage で未然に防ぐ相当）。
    bool m_swallowNextChar = false;

    // --- 色（原 設定オブジェクト +0x08.. 相当のローカル既定） ---
    COLORREF m_clrHeaderText, m_clrHeaderBack;
    COLORREF m_clrAddrText,   m_clrAddrBack;
    COLORREF m_clrDataText,   m_clrDataBack;

    // バイト値別文字色表（原 view+0x40）。既定枝で fg=m_byteColorTable[byteVal]。
    //   ReloadSettings で BuildByteColorTable により再構築（データ文字色＋強調表示コード）。
    COLORREF m_byteColorTable[256];

    // --- 検索状態（原 view+0x178..0x1d0/0x23c/0x240/0x244 相当） ---
    std::vector<unsigned char> m_lastFindPattern; // 直近の検索バイト列（繰り返し検索用）
    int  m_lastFindRange;                       // 直近の検索範囲モード
    bool m_wholeSearchStarted;                  // 「データ全体」検索の初回通過フラグ
    // 直近の検索が不一致検索(0x8032)かどうか。true のとき次/前検索(0x8036/0x8037)は不一致で継続。
    bool m_lastFindMismatch = false;
    unsigned char m_lastFindByte = 0;           // 直近の不一致検索の対象バイト
    // 「選択範囲内」検索の固定範囲（原 view+0x240/0x244）。一致で選択が変わっても不変。
    bool m_findSelCaptured;                     // 固定範囲を確定済みか
    stirling::FileOffset m_findSelLo, m_findSelHi;   // 確定した検索対象範囲 [lo, hi)
    void ResetFindSession();                    // ダイアログを開くたびに検索セッションを初期化

    void CenterCaretRow();   // キャレット行が画面外なら縦中央になるようスクロール（検索用）

    // この文書に適用する表示設定（拡張子で解決した doc の設定。doc 未接続時は theApp 既定）。
    const CStirlingSettings& CurSettings() const;
    // 子フレーム幅をこの文書の設定（1行バイト数・フォント）に合わせて補正する。
    //   既定と異なる拡張子設定でオープンした直後に幅がずれるのを直す（原 PreCreateWindow 相当）。
    void FitFrameWidth();
    void EnsureFont(CDC* pDC);           // フォント生成＋メトリクス確定
    int  AddrDigits() const;             // アドレス欄桁数（基数依存）
    stirling::FileOffset TotalRows() const;   // 総行数（末尾端数行を含む。64bit）
    int  VisibleRows() const;            // クライアント領域に収まるデータ行数（端数行含む）
    int  FullyVisibleRows() const;       // 完全に収まるデータ行数（端数行は含めない。可視判定用）
    void UpdateScrollInfo();             // 縦スクロール範囲の更新
    // 列先頭X（文字セル基準→px）
    int  ColAddrX() const;
    int  ColHexX() const;
    int  ColCharX() const;

    // --- キャレット/編集の補助 ---
    stirling::FileOffset Total() const;      // ドキュメント総バイト数
    stirling::FileOffset CaretRow() const;   // 行番号（総バイト数/1行バイト数のため 64bit）
    int  CaretCol() const;                   // 行内の列（0..bytesPerRow-1）
    void CaretPixel(int& x, int& y) const;  // キャレットセル→クライアント座標
    void CreateCaretForMode();           // 挿入=細線 / 上書き=ブロック の生成
    void UpdateCaret();                  // 位置反映＋可視判定して表示/非表示
    void RefreshSubCaret();              // サブキャレット下線の旧/新位置を局所無効化（subCaret時）
    void EnsureCaretVisible();           // キャレット行が可視になるよう縦スクロール
    // 移動（extend=選択拡張 / 非拡張は選択解除）
    void MoveCaretTo(stirling::FileOffset newPos, bool extend);
    // 編集後: 再計算・全ビュー更新・キャレット反映
    void AfterEdit(stirling::FileOffset newCaretPos);

    // --- キーアサイン（keymap）ディスパッチ（原 FUN_004273ec/FUN_00418b31） ---
    //   押下VKを現修飾状態で内部keycodeへ変換し keymap[modstate*0x40+keycode] を引く。
    //   戻り=機能rawID（0=非マップ）。修飾なしの矢印/PgUp/PgDn等は非マップ（原のゲート準拠）。
    UINT KeymapLookup(UINT vk) const;
    //   rawID を種別で起動: 0x500-0x509=ユーザーメニュー1-10 / 0x50a-0x50c=2ストローク /
    //   その他=rawID→cmdID→WM_COMMAND（各コマンドハンドラへ）。
    void DispatchKeymapRaw(UINT raw);

    // --- ２ストロークキー（原 FUN_00427591/FUN_004275e5） ---
    void StartTwoStroke(int menuIdx);        // 第1打鍵: 保留状態に入りタイマ開始（menuIdx=userMenusの添字）
    bool HandleTwoStrokeSecond(UINT vk);     // 第2打鍵: アクセラレータ照合。処理したら true（Escでキャンセル）
    void CancelTwoStroke();                  // 保留解除（タイマ停止）

    // --- カーソル移動の内部実体（コマンド/既定ナビ共用。ext=選択拡張） ---
    void CaretUpOne(bool ext);           // 単一上（最上段では不動＝原挙動）
    void CaretDownOne(bool ext);         // 単一下（末尾クランプ＝原挙動）
    void CaretLineEndTo(bool ext);       // 行右端（末尾端数行を考慮）
    void ScrollByLines(int lines);       // ライン上下: キャレット不動で lines 行スクロール

    // --- スクロールバーのスケーリング（Issue #21） ---
    //   Win32 の SCROLLINFO は 32bit のため、総行数が INT_MAX を超える場合は比率で写す。
    //   総行数が INT_MAX 以下のときは 1:1（従来と完全に同じ挙動）。
    stirling::FileOffset TotalRowsForScroll() const;   // スクロール用の総行数（>=0）
    int  RowToScrollPos(stirling::FileOffset row) const;
    stirling::FileOffset ScrollPosToRow(int pos) const;

    // --- 選択 ---
    stirling::FileOffset SelLo() const {
        return (m_selAnchor < m_caretPos) ? m_selAnchor : m_caretPos;
    }
    stirling::FileOffset SelHi() const {
        return (m_selAnchor < m_caretPos) ? m_caretPos : m_selAnchor;
    }
    void ClearSelection();               // 選択解除（必要なら再描画）
    bool DeleteSelection();              // 選択範囲を削除しキャレットを先頭へ。削除したら true
    // 選択バイト列を Windows クリップボードへ CF_TEXT で書き出す（原 FUN_0045c53f 相当）
    void SetClipboardText(const std::vector<unsigned char>& bytes);
    // クライアント座標→(行,列,ペイン)。データ領域内なら true。
    // row は画面上の相対行ではなくデータの絶対行番号のため 64bit。
    bool HitTest(CPoint pt, stirling::FileOffset& row, int& col, int& pane) const;

    // pattern を direction 方向・rangeMode で検索し、見つかれば選択して true。
    bool DoSearch(const std::vector<unsigned char>& pattern, bool forward, int rangeMode);
    // 指定1バイトに一致しない最初の位置を direction 方向・rangeMode で検索（不一致検索の実体）。
    //   範囲・セッション決定は DoSearch と同一（原 FUN_0044b654 の不一致分岐）。
    bool DoMismatchSearch(unsigned char value, bool forward, int rangeMode);
    // 検索不一致時のフィードバック（原 FUN_0044b654 末尾）: searchNotFoundMsg(view+0x264)
    //   有効ならメッセージ、無効ならビープ。found が true の時は何もしない。
    void NotifySearchResult(bool found);
    // [lo,hi) 内の search を repl で前方一括置換し、置換件数を返す（原 FUN_0044c0d1 相当）。
    int  ReplaceAll(const std::vector<unsigned char>& search,
                    const std::vector<unsigned char>& repl,
                    stirling::FileOffset lo, stirling::FileOffset hi);

    // バイト毎の前景/背景色を決定（原 GetByteColor 0x45cf92）。
    //   優先順は比較差分→マーク→構造体範囲→強調コード／データ既定色。
    //   検索ヒットは原版同様、この色決定後に選択範囲を反転して表示する。
    bool GetByteColor(stirling::FileOffset absPos, unsigned char byteVal,
                      COLORREF& fg, COLORREF& bg) const;

    // --- データ比較状態（原 view+0x2fc/+0x300 相当） ---
    // 相違範囲[start,end]（昇順）
    std::vector<std::pair<stirling::FileOffset, stirling::FileOffset>> m_compareDiffs;
    bool m_compareActive;                             // 比較モード（差分ハイライト有効）
    bool InCompareDiff(stirling::FileOffset pos) const;   // pos が相違範囲内か（二分探索）
    // シンクロスクロール同期グループ（原 view+0x324）。この配列の各ビューへ先頭行を伝播する。
    //   グループは対称・完全連結（各メンバーの配列＝自分以外の全メンバー）。
    std::vector<CStirlingView*> m_syncGroup;
    CDiffListDlg*  m_pDiffDlg = nullptr;              // 所有する相違一覧ダイアログ（モードレス）
    bool m_checkingFileChange = false;                // 外部変更通知の表示中（再入防止）

    // 構造体編集の表示範囲ハイライト（原 view+0x304/0x308/0x30c）。
    bool m_structHiliteActive = false;                // 有効フラグ
    stirling::FileOffset m_structHiliteStart = 0;     // 範囲開始（両端含む）
    stirling::FileOffset m_structHiliteEnd = -1;      // 範囲終了（両端含む）

    // キャレット位置に種別 type のマークをトグル（未登録→登録/同種別→解除/別種別→変更）。
    //   Mark1/2/3 コマンド共通。末尾(データ無し)では無処理。
    void ToggleMarkAt(int type);

    // 画面描画の二重化用。印刷時は OnDraw から既存の直接描画経路を使う。
    void DrawContent(CDC* pDC);

    // --- 文字欄の文字セット別描画（原 this+0x344 レンダラ群の移植） ---
    //   可視行の文字欄テキストを描画（ASCII/SJIS/EUC/Unicode/EBCDIC/EBCIDK）。
    //   各ソースバイトは必ず1セルを消費し、DBCSペアは2バイト=2セル（二倍幅グリフ）。
    void DrawCharColumn(CDC* pDC, stirling::FileOffset firstRow, int rows,
                        stirling::FileOffset total);
    // buf[gi..] から cols セル分の文字欄テキストを構築（文字セット別。DBCS/繰越し対応）。
    //   gi/carry を更新し、データ末尾に達したら eof=true。表示とダンプ保存で共用。
    std::string BuildCharCells(const std::vector<unsigned char>& buf, int& gi, int cols,
                               int charset, int& carry, bool beBig,
                               const unsigned char* ebc, bool& eof) const;
    // 行頭 abs が多バイト文字の途中（＝先頭に空白を出す繰越し）か判定（行跨ぎDBCS用）。
    int  InitialCarry(int charset, stirling::FileOffset startAbs, stirling::FileOffset total);
    // --- UTF-8 文字欄（charset 6。移植で追加。Issue #98） ---
    //   CP932 に無い文字も表示するためワイド描画にする。不変条件は他と同じで
    //   1 ソースバイト = 1 表示セル。多バイト文字はバイト数ぶんのセルを占め、
    //   グリフが使い切らなかったセルは空白で埋める。
    //   out へ描画するワイド文字列、dx へその各文字の送り幅を積む（ExtTextOutW 用）。
    //   cellsOut は消費した表示セル数（out の文字数とは一致しない）。
    void BuildCharCellsUtf8(const std::vector<unsigned char>& buf, int& gi, int cols,
                            int& carryCells, HDC hdc, int charW,
                            std::vector<unsigned char>* cache,
                            std::wstring& out, std::vector<INT>& dx,
                            int& cellsOut, bool& eof) const;
    // 窓の先頭 startAbs が UTF-8 の多バイト文字の途中なら、読み飛ばすバイト数（0..3）。
    //   そのバイトは窓の中にあるので、同じ数だけ行頭に空白セルを置く。
    int  InitialCarryUtf8(stirling::FileOffset startAbs);
    // UTF-8 文字欄の描画（DEFAULT_CHARSET フォントでワイド描画）。
    void DrawCharColumnUtf8(CDC* pDC, stirling::FileOffset start, int rows, int bpr,
                            const std::vector<unsigned char>& buf, int x);
    // 範囲[startPos,endPos]（両端含む）を整形テキストダンプでファイルへ（原 FUN_0045d3e2）。
    //   Issue #155: 入力・出力とも一定行数ごとのチャンクで処理し、範囲全体や出力テキスト
    //   全体をメモリへ載せない。失敗時はメッセージを表示し、出力先ファイルは変更しない。
    bool WriteDumpImage(const CString& path, stirling::FileOffset startPos,
                        stirling::FileOffset endPos);
    // 範囲[lo,hi) の生バイトを固定サイズのチャンクでファイルへ書き出す（Issue #155）。
    //   成功で true。失敗時はメッセージを表示し、出力先ファイルは変更しない。
    bool WriteRangeToFile(const CString& path, stirling::FileOffset lo,
                          stirling::FileOffset hi);
    // 範囲保存の読み書きチャンク長（選択範囲の大きさに依存しない固定サイズ。Issue #155）。
    static const size_t kSaveChunkBytes = 64u * 1024u;
    // ダンプ保存で 1 回に処理する行数（Issue #155）。
    static const int kDumpChunkRows = 256;
    // 文字欄が行末で次行から先読み・消費し得るバイト数の上限（UTF-8 の 4 バイト文字）。
    //   チャンクの末尾にこの分だけ余分に読み、範囲全体を 1 バッファに載せた場合と
    //   1 バイトも差が出ないようにする。
    static const int kCharLookahead = 3;

    // --- 印刷の内部状態／補助（原 view+0x338=印刷範囲, view+0x44=印刷フォント） ---
    CFont m_printFont;              // 印刷用フォント（ＭＳ明朝 h100 SHIFTJIS。OnBeginPrinting で生成）
    CFont m_printFontUtf8;          // 印刷用 UTF-8 フォント（同寸法・DEFAULT_CHARSET）
    // 全画面プレビュー中、ビューを一時的にメインフレームへ付け替える際の復帰用子フレーム。
    CFrameWnd* m_pPreviewChildFrame = nullptr;
    bool  m_printRangeActive = false;  // 印刷範囲を指定中か（原 view+0x338 != 0）
    stirling::FileOffset m_printRangeStart = 0;   // 印刷範囲 開始（両端含む）
    stirling::FileOffset m_printRangeEnd = 0;     // 印刷範囲 終了（両端含む）
    // 印刷レイアウト（1ページの行数・描画メトリクス）。原はページ矩形を GetDeviceCaps/m_rectDraw から算出。
    struct PrintLayout {
        int charW = 0, rowH = 0;       // 印刷フォントの文字幅／行高
        CRect grid;                    // ダンプグリッド左上（ヘッダ行の原点）とその領域
        int   rowsPerPage = 1;         // 1ページのデータ行数
    };
    PrintLayout ComputePrintLayout(CDC* pDC, const CRect& page) const;   // page=ページ描画矩形
    // 印刷対象の総行数（全体 or 印刷範囲、16境界整列）
    stirling::FileOffset PrintTotalRows() const;
    // 印刷対象の先頭アドレス（範囲なら開始、全体なら0）
    stirling::FileOffset PrintFirstAddr() const;
    // 印刷対象の末尾アドレス（両端含む）
    stirling::FileOffset PrintLastAddr() const;
    // 印刷1バイトの前景/背景色（原 FUN_0045d161）: 既定=黒/白、比較差分=白/黒（反転）。
    void GetPrintByteColor(stirling::FileOffset absPos, COLORREF& fg, COLORREF& bg) const;
    void SelectPrintFont(CDC* pDC);    // 印刷フォントを生成（未生成時）

    // --- 文字セット別入力（原 CStirlingView_TranslateInputChar の移植） ---
    //   入力文字(WM_CHAR/WM_IME_CHAR, 既定コードページ=SJIS)を現文字セットのバイト列へ変換。
    static std::vector<unsigned char> TranslateInputChar(int charset, unsigned int ch);
    //   文字ペインへの1文字入力（変換＋選択置換/挿入/上書きを処理。単一Undoレコード）。
    void InputTextChar(unsigned int ch);

    afx_msg int  OnCreate(LPCREATESTRUCT lpCreateStruct);
    afx_msg void OnDestroy();                            // 比較ダイアログ/同期の後始末
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnDropFiles(HDROP hDropInfo);           // ファイルD&Dでオープン（原 DragQueryFileA）
    afx_msg BOOL OnEraseBkgnd(CDC* pDC);
    afx_msg void OnVScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar);
    afx_msg void OnHScroll(UINT nSBCode, UINT nPos, CScrollBar* pScrollBar);
    afx_msg BOOL OnMouseWheel(UINT nFlags, short zDelta, CPoint pt);
    afx_msg void OnChar(UINT nChar, UINT nRepCnt, UINT nFlags);
    afx_msg void OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags);
    afx_msg void OnTimer(UINT_PTR nIDEvent);            // ２ストロークのタイムアウト→視覚ポップアップ
    afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
    afx_msg void OnMouseMove(UINT nFlags, CPoint point);
    afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
    afx_msg void OnSetFocus(CWnd* pOldWnd);
    afx_msg void OnKillFocus(CWnd* pNewWnd);
    // ユーザーメニュー実適用（原 15メニュー設定）。
    afx_msg void OnContextMenu(CWnd* pWnd, CPoint point);   // 右クリック→userMenus[14]
    afx_msg void OnUserMenuInvoke(UINT nID);
    afx_msg void OnRunApp();   // 名前を指定して実行（0x804f）                // 0x803A-0x8046→userMenus[0..12]
    void PopupUserMenuAtCaret(int idx);                    // userMenus[idx] をキャレット直下に表示
    afx_msg void OnEditUndo();
    afx_msg void OnEditRedo();
    afx_msg void OnUpdateEditUndo(CCmdUI* pCmdUI);
    afx_msg void OnUpdateEditRedo(CCmdUI* pCmdUI);
    afx_msg void OnEditCopy();
    afx_msg void OnEditCut();
    afx_msg void OnEditPaste();
    afx_msg void OnEditPasteHex();                       // 16進テキスト貼り付け（0x80F8。Issue #97）
    afx_msg void OnUpdateEditCopy(CCmdUI* pCmdUI);
    afx_msg void OnUpdateEditCut(CCmdUI* pCmdUI);
    afx_msg void OnUpdateEditPaste(CCmdUI* pCmdUI);
    afx_msg void OnUpdateEditPasteHex(CCmdUI* pCmdUI);
    // 貼り付け本体（内部バイナリ／16進テキストの両経路で共有）。
    //   選択あり=置換／選択なしは上書きモード＋pasteOverwrite なら上書き、それ以外は挿入。
    void PasteBytes(const std::vector<unsigned char>& bytes);
    afx_msg void OnCharset(UINT nID);                    // ID_CHARSET_* 6種を一括受信
    afx_msg void OnCharsetUtf8();                        // ID_CHARSET_UTF8（移植で追加）
    // ワイド入力（UTF-16 コード単位）を 1 文字ぶん処理する。UTF-8 は CP932 を
    //   経由せず直接符号化するため、CP932 に無い文字も入力できる（Issue #98）。
    void InputWideChar(UINT unit);
    // 変換済みバイト列を現在のモードで挿入／上書きする。
    //   bytes は値渡し（上書きモードで末尾に合わせて縮めることがあるため）。
    void InputBytes(std::vector<unsigned char> bytes);
    // UTF-8 入力で保留中の上位サロゲート（0=無し）。WM_CHAR は 2 回に分けて届く。
    wchar_t m_pendingHighSurrogate = 0;
    afx_msg void OnUpdateCharset(CCmdUI* pCmdUI);        // ラジオチェック
    afx_msg void OnByteOrder(UINT nID);                  // ID_BYTEORDER_LITTLE/BIG
    afx_msg void OnUpdateByteOrder(CCmdUI* pCmdUI);
    afx_msg LRESULT OnImeChar(WPARAM wParam, LPARAM lParam);  // IME確定の全角文字入力
    // 外部プロセスによるファイル変更の確認（原 WM_USER+0x1B = 0x041B のハンドラ）。
    afx_msg LRESULT OnCheckFileChanged(WPARAM wParam, LPARAM lParam);
    afx_msg void OnMarkToggle();
    afx_msg void OnMark2Toggle();                       // マーク2登録／解除（0x8062, 種別1）
    afx_msg void OnMark3Toggle();                       // マーク3登録／解除（0x8063, 種別2）
    afx_msg void OnUpdateMarkToggle(CCmdUI* pCmdUI);   // 末尾(データ無し)では無効化（Mark1/2/3 共通）
    afx_msg void OnMarkNext();
    afx_msg void OnMarkPrev();
    afx_msg void OnMarkClearAll();
    afx_msg void OnUpdateMarkExists(CCmdUI* pCmdUI);   // next/prev/clear の活性
    afx_msg void OnMarkList();                          // マーク一覧ダイアログを開く
    afx_msg void OnMarkExport();                        // マークをファイルへ書き出す（Issue #99）
    afx_msg void OnMarkImport();                        // マークをファイルから読み込む（Issue #99）
    // --- カーソル移動コマンド（原 cat1。keymap 経由で起動。ext=m_selectMode） ---
    afx_msg void OnCursorLeft();                        // 0x800a カーソル左
    afx_msg void OnCursorRight();                       // 0x800b カーソル右
    afx_msg void OnCursorUp();                          // 0x800c カーソル上
    afx_msg void OnCursorDown();                        // 0x800d カーソル下
    afx_msg void OnCursorLineHome();                    // 0x8010 行左端に移動
    afx_msg void OnCursorLineEnd();                     // 0x8011 行右端に移動
    afx_msg void OnCursorFastUp();                      // 0x8012 高速上移動（±2行）
    afx_msg void OnCursorFastDown();                    // 0x8013 高速下移動（±2行）
    afx_msg void OnPageUp();                            // 0x8014 ページアップ
    afx_msg void OnPageDown();                          // 0x8015 ページダウン
    afx_msg void OnHalfPageUp();                        // 0x8016 半ページアップ（±可視行/2）
    afx_msg void OnHalfPageDown();                      // 0x8017 半ページダウン（±可視行/2）
    afx_msg void OnLineUp();                            // 0x8018 ラインアップ（1行スクロール）
    afx_msg void OnLineDown();                          // 0x8019 ラインダウン（1行スクロール）
    // --- 選択拡張コマンド（原 cat2。always extend=true） ---
    afx_msg void OnSelectMode();                        // 0x801c 選択モード開始／終了
    afx_msg void OnSelectLeft();                        // 0x801d 選択左
    afx_msg void OnSelectRight();                       // 0x801e 選択右
    afx_msg void OnSelectUp();                          // 0x801f 選択上
    afx_msg void OnSelectDown();                        // 0x8020 選択下
    afx_msg void OnSelectDataTop();                     // 0x8021 データ先頭まで選択
    afx_msg void OnSelectDataEnd();                     // 0x8022 データ末尾まで選択
    afx_msg void OnSelectLineHome();                    // 0x8023 行左端まで選択
    afx_msg void OnSelectLineEnd();                     // 0x8024 行右端まで選択
    afx_msg void OnSelectPageUp();                      // 0x805d 前１ページ分選択
    afx_msg void OnSelectPageDown();                    // 0x805e 次１ページ分選択
    // --- 編集トグル/削除コマンド（原 cat3） ---
    afx_msg void OnToggleInsert();                      // 0x8026 上書／挿入切替
    afx_msg void OnTogglePane();                        // 0x8027 数値入力／文字入力切替
    afx_msg void OnDeleteByte();                        // 0x8028 １バイト削除
    afx_msg void OnDeleteByteBack();                    // 0x8029 直前の１バイト削除
    afx_msg void OnGotoDataTop();                       // データ先頭へ移動
    afx_msg void OnGotoDataEnd();                       // データ末尾へ移動
    afx_msg void OnJump();                              // 指定アドレスへ移動ダイアログ
    afx_msg void OnGotoLastModified();                  // 最終変更箇所へ移動
    afx_msg void OnUpdateGotoLastModified(CCmdUI* pCmdUI);
    afx_msg void OnEditFind();                          // 検索ダイアログを開く
    afx_msg void OnFindMismatch();                      // 不一致検索ダイアログを開く（0x8032）
    afx_msg void OnSyncScroll();                        // シンクロスクロール手動登録ダイアログ（0x805f, IDD193）
    afx_msg void OnAdjustWindowSize();                  // ウィンドウサイズ補正（0x8049）: 子フレームを内容幅へ整える
    afx_msg void OnEditReplace();                        // 置換ダイアログを開く
    afx_msg void OnUpdateEditReplace(CCmdUI* pCmdUI);    // 編集禁止中は無効
    afx_msg void OnFindNextCmd();                       // 繰り返し検索(前方)
    afx_msg void OnFindPrevCmd();                       // 繰り返し検索(後方)
    afx_msg void OnDeleteSelection();                   // 選択範囲の削除（0x802a）
    afx_msg void OnFillSelection();                     // 選択範囲の初期化（0x802b, IDD 165）
    afx_msg void OnSaveSelection();                     // 選択範囲を生バイナリで保存（0x802c）
    afx_msg void OnUpdateSelectionCmd(CCmdUI* pCmdUI);  // 選択あり時のみ活性（削除/初期化/保存 共通）
    afx_msg void OnSaveDump();                          // ダンプイメージの保存（0x8060, IDD 198）
    afx_msg void OnPrintRange();                        // 範囲を指定して印刷（0x8064, IDD 201）
    afx_msg void OnSelectRange();                       // 範囲を指定して選択（0x8065, IDD 202）
    afx_msg void OnToggleReadOnly();                    // 編集禁止／許可の切替（0x8025）
    afx_msg void OnUpdateToggleReadOnly(CCmdUI* pCmdUI);
    afx_msg void OnCompare();                           // データ比較（0x8038, IDD 166）
    // 比較の実行本体（対象ビューを指定）。選択ダイアログと外部変更通知の双方から使う。
    void RunCompareWith(CStirlingView* pTargetView);
    afx_msg void OnUpdateCompare(CCmdUI* pCmdUI);       // 文書2つ以上で有効
    afx_msg void OnEditSelectAll();                     // 全て選択（ID_EDIT_SELECT_ALL 57642）
    afx_msg void OnUpdateEditSelectAll(CCmdUI* pCmdUI); // データ有りで有効
    afx_msg void OnRevertFile();                        // 編集前に戻す（0x802d）
    afx_msg void OnUpdateRevertFile(CCmdUI* pCmdUI);    // CanEdit && 変更あり && パスあり
    afx_msg void OnUpdateEditSelectionCmd(CCmdUI* pCmdUI);  // 削除/初期化: CanEdit && 選択あり
    // ステータスバー各ペインの更新（原 FUN_00424xxx 相当。アイドル時に呼ばれる）
    afx_msg void OnUpdateIndicatorAddress(CCmdUI* pCmdUI);   // 0x%08X キャレット位置16進
    afx_msg void OnUpdateIndicatorAddrDec(CCmdUI* pCmdUI);   // キャレット位置10進
    afx_msg void OnUpdateIndicatorModified(CCmdUI* pCmdUI);  // 「更新」変更あり時
    afx_msg void OnUpdateIndicatorEditLock(CCmdUI* pCmdUI);  // 「編禁」編集禁止時
    afx_msg void OnUpdateIndicatorMode(CCmdUI* pCmdUI);      // 「上書」/「挿入」
    afx_msg void OnUpdateIndicatorSize(CCmdUI* pCmdUI);      // 「%d Bytes」総サイズ10進
    afx_msg void OnUpdateIndicatorSizeHex(CCmdUI* pCmdUI);   // 「0x%08X Bytes」総サイズ16進
    afx_msg void OnUpdateIndicatorCharset(CCmdUI* pCmdUI);   // 文字セット名
    afx_msg void OnUpdateIndicatorByteOrder(CCmdUI* pCmdUI); // 「Little/Big Endian」
    afx_msg void OnUpdateIndicatorByteDec(CCmdUI* pCmdUI);   // 「B : %d」BYTE値10進
    afx_msg void OnUpdateIndicatorByteHex(CCmdUI* pCmdUI);   // 「B : 0x%02X」BYTE値16進
    afx_msg void OnUpdateIndicatorWordDec(CCmdUI* pCmdUI);   // 「W : %d」WORD値10進
    afx_msg void OnUpdateIndicatorWordHex(CCmdUI* pCmdUI);   // 「W : 0x%04X」WORD値16進
    afx_msg void OnUpdateIndicatorDwordDec(CCmdUI* pCmdUI);  // 「DW : %u」DWORD値10進
    afx_msg void OnUpdateIndicatorDwordHex(CCmdUI* pCmdUI);  // 「DW : 0x%08X」DWORD値16進
    afx_msg void OnUpdateIndicatorFloat(CCmdUI* pCmdUI);     // 「f : %g」float値
    afx_msg void OnUpdateIndicatorDouble(CCmdUI* pCmdUI);    // 「d : %g」double値
    // キャレット位置から最大 want バイトを読取り（総サイズ超過分は除外）。戻り=実読取数。
    //   ペイン値表示(BYTE/WORD/DWORD/float/double)用。0 のときキャレットはデータ外。
    int  ReadBytesAtCaret(int want, unsigned char out[8]);
    DECLARE_MESSAGE_MAP()
};
