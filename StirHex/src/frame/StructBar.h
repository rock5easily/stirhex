// CStructBar — 構造体編集バー。
//   原の構造体編集ウィンドウ（CMainFrame+0x390。上/下ドッキングまたは
//   フローティング）の相当機能を、CDialogBar（テンプレート 250）＋標準
//   CListCtrl（仮想レポート）で再現する。Struct.def をパースし、選択構造体の
//   各フィールドをアクティブビューのキャレット位置のデータから型・バイトオーダー
//   に従って解釈表示し、編集書き戻し・ナビ（<< < 移動 > >>）を提供する。
//   CListCtrl は3列全幅・行単位罫線・階層記号を独自描画し、DDS2CustomCtrl相当の
//   ツリーグリッド操作（展開、親子キー移動、値編集）を再現する。
#pragma once

#include "core/CoreTypes.h"   // stirling::FileOffset（アドレスの 64bit 化。Issue #21）

#include "core/StructDef.h"

#include <vector>
#include <string>
#include <set>
#include <map>
#include <array>

class CStirlingView;
class CStructBar;

// ドック境界のマウス押下を構造体バーへ転送する細いリサイズグリップ。
class CStructResizeGrip : public CStatic {
public:
    void SetOwner(CStructBar* owner) { m_owner = owner; }

protected:
    CStructBar* m_owner = nullptr;
    afx_msg BOOL OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message);
    afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
    DECLARE_MESSAGE_MAP()
};

// 構造体編集バーのコントロールバー ID（他の IDW_* と衝突しない値）。
#define IDW_STRUCT_BAR  0xE822

class CStructBar : public CDialogBar {
    friend class CStructResizeGrip;
public:
    CStructBar();

    // ドッキングバー生成（親＝メインフレーム）。既定は非表示。
    BOOL CreateBar(CWnd* pParent);

    // ステータス位置（0=下/1=上/2=非表示）と列幅比率保持を反映する。
    void ApplyDisplaySettings(int statusPos, bool keepColumnRatio);

    // メインフレームのサイズ変更後、実可視幅へ子一覧を再配置する。
    void FitToFrame();

    // Enter/Esc を編集ボックスで捕捉するため（既定ボタン起動を防ぐ）。
    virtual BOOL PreTranslateMessage(MSG* pMsg);
    virtual void OnUpdateCmdUI(CFrameWnd* pTarget, BOOL bDisableIfNoHndler);

    // Struct.def を再パースし、コンボ再構築＋一覧を更新（再読込ボタン相当）。
    void ReloadDefs();

    // 表示化時に呼ぶ: アクティブビューへ束縛し、基準アドレスをキャレット位置へ同期。
    //   原 FUN_00428720（バー表示化時に base←caret）相当。
    void SyncToCaret();

    // 現在の基準アドレス(m_base)/文字セット/バイトオーダーで一覧を再構築。
    //   原はキャレットに自動追従せず、base はナビ（<< < 移動 > >>）でのみ動く。
    //   force=false のときは状態に変化が無ければ何もしない（タイマー駆動用）。
    void Refresh(bool force);

    // データビューの構造体表示範囲ハイライトを解除（バー非表示化時に呼ぶ）。
    void ClearViewHighlight();

    // 「キャレット位置を構造体編集」（原 0x8061）。基準アドレスを現在のキャレット位置へ。
    void SetBaseToCaret();

    // ビュー破棄時に参照を無効化（解放後アクセス防止。デリファレンスしない）。
    void NotifyViewDestroyed(CStirlingView* view);

protected:
    // 横ドッキング時はフレーム幅まで伸ばし、フローティング時はテンプレート寸法を使う。
    virtual CSize CalcFixedLayout(BOOL bStretch, BOOL bHorz);

    // ツリーを可視行へ平坦化した 1 表示行。
    struct DispRow {
        std::string type;    // [byte層] struct.def 由来の CP932 バイト列
        std::string name;    // [byte層] 同上
        // [wide層] 値はそのまま描画する（UTF-8 の char 配列は CP932 に無い文字を含む）。
        std::wstring value;
        std::string path;       // 展開状態キー（安定・一意）
        int  depth = 0;
        bool hasChildren = false;
        bool expanded = false;
        // 編集用（スカラ葉のみ）
        bool editable = false;
        int  offset = -1;       // struct base からの相対オフセット
        int  size = 0;
        stirling::FieldKind kind = stirling::FieldKind::Unknown;
    };

    void SetupList();          // 列（型/シンボル名/値）を作成
    void PopulateCombo();      // コンボへ構造体名を投入（ItemData=定義インデックス）
    void RebuildVisible();     // m_root＋展開状態から m_rows を再構築しリスト更新
    void Flatten(const std::vector<stirling::StructNode>& nodes, int depth,
                 const std::string& parentPath);
    void Toggle(const std::string& path);   // 展開/折り畳みを反転
    void BeginEdit(int row);   // 値セルのインプレース編集を開始（編集可の葉のみ）
    void CommitEdit();         // 編集確定: 解析→データ書き戻し（単一Undo）→再表示
    void CancelEdit();         // 編集取消（破棄）
    int  CurStructSize();      // 選択中構造体のサイズ（無選択は 0）
    // base の上限（構造体が収まる範囲）
    stirling::FileOffset MaxBase(stirling::FileOffset total, int structSize) const;
    // base をクランプして設定＋再表示（不可はビープ）
    void MoveBase(stirling::FileOffset newBase);
    void UpdateAddrStatic();   // 基準アドレス静的を "%08X" で更新
    void CaptureInitialLayout();
    void LayoutChildren();
    void UpdateStructStatus();
    void CaptureColumnRatio();
    void CaptureManualColumnRatioIfChanged();
    void ApplyColumnRatio(int newListWidth);
    void SelectListRow(int row);
    int  DockPosition() const;          // 0=下/1=上/2=フローティング
    bool IsResizeGrip(CPoint point) const;
    void BeginDockedResize();
    void QueueDeferredLayout();
    CStirlingView* ActiveView();  // アクティブ MDI 子のビュー（無ければ nullptr）
    CListCtrl* List();
    CComboBox* Combo();
    static CStringW DefPath();  // Struct.def のフルパス（exe ディレクトリ, ワイド）

    stirling::StructDefSet   m_defs;
    stirling::StructNode     m_root;        // 現在の解釈ツリー
    std::set<std::string>    m_expanded;    // 展開中ノードのパス（refresh 跨ぎで保持）
    std::vector<DispRow>     m_rows;        // 可視行（仮想リストのソース）

    // 表示基数の全体上書き（原 this+0x250。「一括基数指定」）。
    //   -1=型ごとの既定 / 0=符号付10進 / 1=符号なし10進 / 2=16進。全スカラ葉に一律適用
    //   （float/double は基数に依らず浮動小数表示）。構造体を切り替えても保持される（原実測）。
    int   m_radixOverride = -1;

    // 「個別基数指定」で行ごとに上書きした基数（表示行パス→基数。-1=その型の既定へ戻す）。
    //   一括指定は全項目への一律代入のため、実行時にこのマップを空にする（原実測）。
    //   アドレス移動を跨いで保持し、構造体の切り替えでは破棄する（原はツリー再構築で失われる）。
    std::map<std::string, int> m_radixByPath;

    // BuildTree（型ごとの既定基数）の結果へ、一括／個別の基数上書きを反映して値を再整形する。
    void ApplyRadixOverrides(const std::vector<unsigned char>& bytes, bool big,
                             std::vector<stirling::StructNode>& nodes,
                             const std::string& parentPath);
    void SetRadixAll(int radix);                          // 一括基数指定（-1=型ごとの既定）
    void SetRadixItem(const DispRow* row, int radix);     // 個別基数指定（-1=型ごとの既定）

    // 独立した基準アドレス（原 this+0x4a0）。キャレットには自動追従しない。
    stirling::FileOffset m_base = 0;                 // 構造体先頭のデータオフセット
    void* m_boundView = nullptr;            // 束縛中のビュー（切替検出で base 再同期）

    // curPosToStructAddr が OFF のとき、同期点で用いるビューごとの永続構造体アドレス
    //   （原 view+0x304）。ON のときはキャレット位置を使うためこの値は参照しない。
    std::map<CStirlingView*, stirling::FileOffset> m_savedBase;
    // 同期点（表示化/ビュー切替）での base を決定する。
    //   ON=キャレット位置 / OFF=そのビューの永続アドレス（原 FUN_0045d2c8 の分岐）。
    stirling::FileOffset SyncBaseFor(CStirlingView* view) const;
    // ユーザ操作で base が変わったとき、現ビューの永続アドレスへ保存する。
    void  RememberBase(CStirlingView* view);
    CStirlingView* m_hiliteView = nullptr;  // 構造体範囲ハイライトを設定済みのビュー

    // 値のインプレース編集
    CEdit m_editCtrl;                       // 値セル上の編集ボックス（CreateBar で生成）
    CImageList m_rowHeightImages;           // DDS2相当の行高をListViewへ設定する空イメージリスト
    CStructResizeGrip m_resizeGrip;         // 上下ドック時の高さ変更用6px境界
    int   m_editRow = -1;                   // 編集中の可視行（-1=非編集）
    bool  m_committing = false;             // 確定処理中（KillFocus 再入防止）

    // 変化検出用シグネチャ（不要な再構築/ちらつき回避）。
    void* m_lastView   = nullptr;
    stirling::FileOffset m_lastBase   = -1;
    int   m_lastCharset = -1;
    int   m_lastBig    = -1;
    long  m_lastSeq    = -1;
    int   m_lastSel    = -1;

    // バー内レイアウト。テンプレート初期座標を基準に、ステータス位置とバー幅へ追従する。
    static const size_t kHeaderControlCount = 10;
    std::array<CRect, kHeaderControlCount> m_headerRects;
    CRect m_initialListRect;
    CRect m_initialClientRect;
    std::array<int, 3> m_statusWidths = { 0, 0, 0 };
    int  m_statusHeight = 0;
    int  m_statusPos = 2;
    bool m_keepColumnRatio = false;
    std::array<int, 3> m_ratioColumnWidths = { 70, 170, 150 };
    int  m_ratioListWidth = 0;
    int  m_lastLaidOutListWidth = 0;
    bool m_applyingColumnRatio = false;
    bool m_columnsInitialized = false;
    bool m_layoutReady = false;
    bool m_deferredLayoutPosted = false;
    int  m_dockedHeight = 0;
    bool m_resizingDockedHeight = false;
    int  m_resizeStartY = 0;
    int  m_resizeStartHeight = 0;
    int  m_resizeDockPosition = -1;          // ドラッグ開始時に固定（0=下/1=上）
    int  m_layoutDockPosition = 2;           // 直近レイアウトで確定した位置

    afx_msg void OnReload();
    afx_msg void OnSelChange();
    afx_msg void OnCloseButton();
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnShowWindow(BOOL bShow, UINT nStatus);
    afx_msg void OnWindowPosChanged(WINDOWPOS* lpwndpos);
    afx_msg BOOL OnSetCursor(CWnd* pWnd, UINT nHitTest, UINT message);
    afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
    afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
    afx_msg void OnMouseMove(UINT nFlags, CPoint point);
    afx_msg void OnCaptureChanged(CWnd* pWnd);
    afx_msg LRESULT OnDeferredLayout(WPARAM wParam, LPARAM lParam);
    afx_msg void OnNavPrevRec();   // "<<" base -= 構造体サイズ
    afx_msg void OnNavPrevByte();  // "<"  base -= 1
    afx_msg void OnNavGoto();      // "移動" アドレス／マーク位置指定ダイアログ
    afx_msg void OnNavNextByte();  // ">"  base += 1
    afx_msg void OnNavNextRec();   // ">>" base += 構造体サイズ
    afx_msg void OnTimer(UINT_PTR nIDEvent);
    afx_msg void OnGetDispInfo(NMHDR* pNMHDR, LRESULT* pResult);
    afx_msg void OnCustomDraw(NMHDR* pNMHDR, LRESULT* pResult);   // 3列＋ツリー記号＋行罫線を描画
    afx_msg void OnClickList(NMHDR* pNMHDR, LRESULT* pResult);    // [+]/[-] クリックで展開
    afx_msg void OnRClickList(NMHDR* pNMHDR, LRESULT* pResult);   // 右クリック=表示基数変更メニュー
    afx_msg void OnDblClkList(NMHDR* pNMHDR, LRESULT* pResult);   // 行ダブルクリック=展開/編集
    afx_msg void OnKeyDownList(NMHDR* pNMHDR, LRESULT* pResult);  // ←/→/+/-/F2/Enter
    afx_msg void OnEditKillFocus();   // 編集ボックスのフォーカス喪失で確定
    DECLARE_MESSAGE_MAP()
};
