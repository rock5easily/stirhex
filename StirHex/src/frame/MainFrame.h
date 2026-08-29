// CMainFrame — MDI メインフレーム（原 CMainFrame : CMDIFrameWnd）。
#pragma once

#include "frame/StructBar.h"
#include "frame/OutputBar.h"
#include "frame/BitImageBar.h"
#include "dialog/BgrepDlg.h"   // BgrepSettings

class CStirlingView;
class CStirlingDoc;

class CMainFrame : public CMDIFrameWnd {
    DECLARE_DYNAMIC(CMainFrame)
public:
    CMainFrame();
    virtual ~CMainFrame();

    void UpdateTitle() { OnUpdateFrameTitle(TRUE); }   // 変更マーク反映のため外部から起動

    // ビュー破棄時に構造体編集バーの参照を無効化（解放後アクセス防止）。
    void NotifyViewDestroyed(CStirlingView* view) { m_wndStructBar.NotifyViewDestroyed(view); }

    // アウトプットペインへの結果追加/クリア（BGREP 実行から使用）。
    CStirlingOutputBar& OutputBar() { return m_wndOutputBar; }

    // 環境設定変更後などに外部からツールバー/バー表示を再適用する。
    void ApplyBarSettings();

    // 文書データが変わったときに呼ぶ（原 AdjustPositionsAfterEdit 経由の追従に相当）。
    //   実際の再構築はメッセージへ遅延させ、全置換のような一括編集でも1回にまとめる。
    void QueueBitImageRefresh();

    // ビットイメージが表示中ならアクティブ文書から再構築（realtimeBitImage の即時反映で
    //   ビュー側の編集後に呼ばれる）。
    void RefreshBitImage();

    // メインウィンドウ配置確定後、ビットイメージを原版相当位置へフローティングする。
    void FinalizeInitialBitImagePlacement();

protected:
    CToolBar   m_wndToolBar;     // メインツールバー（toolbarItems から動的構築。原リソース128）
    CStatusBar m_wndStatusBar;
    CStructBar m_wndStructBar;   // 構造体編集バー（既定非表示。0x802e/0x80f6 でトグル）
    bool m_bitImageRefreshPending = false;   // 遅延再構築の投函済みフラグ（重複投函の抑止）
    CStirlingOutputBar m_wndOutputBar;   // アウトプットペイン（既定非表示。0x80e8 でトグル）
    CBitImageBar m_wndBitImageBar;       // ビットイメージ・ペイン（既定非表示。0x80eb でトグル）
    BgrepSettings m_bgrepSettings;       // BGREP 設定（ダイアログ跨ぎで保持）

    // 現在アクティブな Stirling 文書（ビットイメージ再構築用。無ければ nullptr）。
    CStirlingDoc* ActiveStirlingDoc();

    // タイトル末尾に編集マーク「 *」を付与（原挙動: 活性文書が変更あり時）。
    virtual void OnUpdateFrameTitle(BOOL bAddToTitle);

    // toolbarItems（アプリ設定）からツールバーのボタン列を再構築する（原の動的構築相当）。
    void RebuildToolbar();

    // statusItems（アプリ設定）からステータスバーのペイン列を再構築する（原の動的構築相当）。
    //   先頭はメッセージ行（ID_SEPARATOR）。各項目IDの幅はRC文字列(59136-59158)から自動算出。
    void RebuildStatusBar();

    // 構造体編集バーの初期配置・ドッキング可否・内部表示設定を反映する。
    void ApplyStructBarSettings();

    // bitImageDockable を現在位置を変えずにドッキング可否へ反映する。
    void ApplyBitImageBarSettings();

    // 相違一覧のアプリ内最小化プロキシを、MDIクライアント領域の変化へ追従させる（Issue #123）。
    virtual void RecalcLayout(BOOL bNotify = TRUE) override;

    afx_msg int OnCreate(LPCREATESTRUCT lpCreateStruct);
    afx_msg void OnDestroy();
    afx_msg BOOL OnCopyData(CWnd* pWnd, COPYDATASTRUCT* pCopyDataStruct);
    afx_msg void OnSize(UINT nType, int cx, int cy);
    // 終了時、winPlacement=1(前回終了時)なら現在の通常配置を設定へ保存する（原ヘルプ準拠）。
    afx_msg void OnClose();
    afx_msg void OnDropFiles(HDROP hDropInfo);   // D&Dでオープン（リンク解決しない。原ヘルプ準拠）
    // 構造体編集バーの表示トグル（原 FUN_004268c1）。編集/設定 両メニューから同一動作。
    afx_msg void OnStructBarToggle();
    afx_msg void OnUpdateStructBarToggle(CCmdUI* pCmdUI);  // 可視状態でチェック（原 FUN_00428343）
    afx_msg void OnStructBarCaret();   // キャレット位置を構造体編集（0x8061）
    afx_msg void OnRunApp();           // 名前を指定して実行（0x804f）
    // 遅延させたビットイメージ再構築（QueueBitImageRefresh からポストされる）。
    afx_msg LRESULT OnBitImageRefreshQueued(WPARAM wParam, LPARAM lParam);
    // アウトプットペイン: トグル(0x80e8)＋コンテキスト操作(タグジャンプ/コピー/クリア)。
    afx_msg void OnOutputPaneToggle();
    afx_msg void OnUpdateOutputPaneToggle(CCmdUI* pCmdUI);  // 可視状態でチェック
    afx_msg void OnOutputTagJump();       // 0x80ea: 選択行を開く
    afx_msg void OnOutputCopy();          // 0x80f7: 検索結果をコピー
    afx_msg void OnOutputClear();         // 0x80e9: クリア
    afx_msg void OnUpdateOutputHasResults(CCmdUI* pCmdUI);  // 結果ありで活性
    afx_msg void OnBgrep();               // 0x8039: BGREP 設定→実行
    // ビットイメージ・ペイン: トグル(0x80eb)＋最新イメージ表示(0x80ec)。
    afx_msg void OnBitImageToggle();
    afx_msg void OnUpdateBitImageToggle(CCmdUI* pCmdUI);    // 可視状態でチェック（原 FUN_004282d2）
    afx_msg void OnBitImageLatest();      // 0x80ec: 現アクティブ文書から再構築
    // ステータスバー各ペインのフォールバック更新（アクティブビューが無い＝未オープン時のみ到達）。
    //   原は未オープン時「レディ」以外は空欄。テンプレート文字列の残留を防ぐためペインを無効化する。
    afx_msg void OnUpdateIndicatorEmpty(CCmdUI* pCmdUI);
    // 環境設定（0x8050）: アプリ全体の動作環境をプロパティシートで編集→OKで保存。
    afx_msg void OnSettingsEnv();
    // 拡張子別設定（0x8051）: 拡張子レコード一覧を編集し、全ビューへ再適用＋保存。
    afx_msg void OnSettingsExt();
    DECLARE_MESSAGE_MAP()
};
