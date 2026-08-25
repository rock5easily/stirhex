// CStirlingDoc — ドキュメント（原 CStirlingDoc : CDocument）。
// core の BlockList を保持し、Load/Save を BlockFileIO 経由で行う。
// Undo/Redo、マーク、文字セット、バイトオーダ、拡張子別表示設定を保持・提供する。
#pragma once

#include "core/BlockList.h"
#include "core/CoreTypes.h"
#include "app/StirlingSettings.h"
#include "util/ScopedHandle.h"

#include <vector>
#include <map>

class CStirlingDoc : public CDocument {
    DECLARE_DYNCREATE(CStirlingDoc)
public:
    CStirlingDoc();
    virtual ~CStirlingDoc();

    // CDocument オーバーライド
    virtual BOOL OnNewDocument();
    virtual BOOL OnOpenDocument(LPCTSTR lpszPathName);
    virtual BOOL OnSaveDocument(LPCTSTR lpszPathName);
    virtual void DeleteContents();
    virtual void Serialize(CArchive& ar);
    // 変更フラグ変化時に子フレームのタイトル（編集マーク「*」）を更新する。
    virtual void SetModifiedFlag(BOOL bModified = TRUE);
    // パス設定時にタイトルを決定（原ヘルプ「ドキュメントのフルパス表示」docFullPath）。
    //   ON ならタイトルをフルパスに、OFF なら MFC 既定の短いファイル名にする。
    virtual void SetPathName(LPCTSTR lpszPathName, BOOL bAddToMRU = TRUE);
    // docFullPath 変更時に現在のパスからタイトルを再決定する（環境設定 OK 後の全文書再適用）。
    void RefreshDocTitle();

    // 編集前に戻す（原 FUN_0043621f）: 保存済みファイルを再読込し編集内容を破棄。パス無しは false。
    bool RevertToSaved();

    // --- 外部プロセスによるファイル変更の検知（原 doc+0x330 の最終更新日時保持） ---
    //   読込/保存のたびにディスク上の更新日時を控え、ビューのアクティブ化時に突き合わせる。
    //   ファイルを開けない場合（削除された等）は「変更なし」として扱う（原の実測挙動）。
    bool HasExternalChange() const;
    // 現在のディスク上の更新日時を控え直す（通知後に再表示させないため）。
    void SyncDiskTime();

    // データ変更シーケンス番号（編集/Undo/Redo/保存等で単調増加）。
    //   構造体編集バー等が「キャレット非移動の書換え」を検出して再表示するために用いる。
    long ChangeSeq() const { return m_changeSeq; }

    // View からのアクセス
    stirling::BlockList& Blocks() { return m_blocks; }
    stirling::FileOffset GetTotalLength() const { return m_blocks.GetTotalLength(); }

    // --- 編集モード（原 doc+0x73c: 挿入/上書き。コマンド0x8026・Insertキーで切替） ---
    bool IsOverwriteMode() const { return m_overwriteMode; }
    void SetOverwriteMode(bool ow) { m_overwriteMode = ow; }

    // --- 編集可否状態（原 doc+0x64: 0=ロック / 1=編集禁止 / 2=編集可。既定=編集可） ---
    //   コマンド 0x8025「編集禁止／許可の切替」で 2↔1 をトグル。CanEdit で全編集操作をゲート。
    int  EditState() const { return m_editState; }
    bool CanEdit() const { return m_editState == 2; }
    // 編集禁止/許可を切替（原 FUN_00436ce5）。状態0はロックで切替不可（false）。2↔1 を反転し true。
    bool ToggleEditState();

    // --- 文字セット（原 doc+0x744。0=ASCII/1=SJIS/2=EUC/3=Unicode/4=EBCDIC/5=EBCIDK） ---
    //   原の既定は 1(SJIS)（ctor で doc+0x744=1）。表示（文字欄）に用いる。
    int  GetCharset() const { return m_charset; }
    void SetCharset(int cs) { if (cs >= 0 && cs <= 5) { m_charset = cs; } }
    // --- バイトオーダ（Unicode 表示の 2バイト解釈。false=リトル/true=ビッグ） ---
    bool IsByteOrderBig() const { return m_byteOrderBig; }
    void SetByteOrderBig(bool big) { m_byteOrderBig = big; }

    // --- 編集プリミティブ（絶対バイト位置。成功で SetModifiedFlag＋Undo記録し true） ---
    //   原の InsertByteAtCaret / OverwriteByteAtCaret / DeleteByteAtCaret / DeleteBytes に相当。
    // x64 化(Issue #21): 位置・長さは core と同じ stirling::FileOffset(64bit)。
    bool InsertByteAt(stirling::FileOffset pos, unsigned char b);
    // pos<総長は in-place / EOF は追記
    bool OverwriteByteAt(stirling::FileOffset pos, unsigned char b);
    bool DeleteByteAt(stirling::FileOffset pos, unsigned char* outByte);
    // 範囲削除（1レコード。選択削除用）
    //   注: 削除バイト列を Undo レコードへ退避するため同容量のメモリを消費する（Issue #30）。
    bool DeleteRange(stirling::FileOffset pos, stirling::FileOffset count);
    // 範囲置換（1レコード。原 ReplaceRange 0xc00009。選択範囲へ入力＝選択削除+挿入を単一Undo単位に）
    bool ReplaceRange(stirling::FileOffset pos, stirling::FileOffset delLen,
                      const std::vector<unsigned char>& ins);
    // 読取（キャレット表示/ニブル合成用）
    bool GetByteAt(stirling::FileOffset pos, unsigned char* outByte);
    // 範囲読取（Copy/Cut 用）。pos から count バイトを実データ範囲までクランプして返す。
    //   戻り値はメモリ上のバッファのため、要求長はメモリに収まる範囲であること。
    std::vector<unsigned char> ReadRange(stirling::FileOffset pos, stirling::FileOffset count);
    // in-place 書換（Undo記録なし）。16進の下位ニブル確定で使用し、1バイト＝1レコードの
    // Undo粒度を原に合わせる（上位ニブルの1レコードに下位ニブルを畳み込む）。
    bool SetByteNoUndo(stirling::FileOffset pos, unsigned char b);

    // --- Undo/Redo（原 doc+0x88=Undo / doc+0x9c=Redo スタックの逆操作レコード方式, doc11） ---
    //   Undo()/Redo() は編集が起きた絶対位置を返す（何も無ければ -1）。
    stirling::FileOffset Undo();
    stirling::FileOffset Redo();
    bool CanUndo() const { return !m_undoStack.empty(); }
    bool CanRedo() const { return !m_redoStack.empty(); }
    // 最終変更箇所（原 doc+0x88 Undoスタック先頭の編集位置）。無ければ -1。
    bool HasLastModified() const { return !m_undoStack.empty(); }
    stirling::FileOffset LastModifiedPos() const {
        return m_undoStack.empty() ? -1 : m_undoStack.back().pos;
    }

    // --- マーク（原 doc+0x748 マップ。位置→種別0/1/2＝Mark1/2/3。GetByteColor で使用） ---
    //   ToggleMark: 未登録→登録 / 同種別登録済→解除 / 別種別→種別変更（原 FUN_0042943a）。
    void ToggleMark(stirling::FileOffset pos, int type);
    // pos より後の最初のマーク位置（無ければ先頭へラップ）。空は -1
    stirling::FileOffset NextMark(stirling::FileOffset pos) const;
    // pos より前の最後のマーク位置（無ければ末尾へラップ）。空は -1
    stirling::FileOffset PrevMark(stirling::FileOffset pos) const;
    void ClearMarks();
    // pos にマークがあれば種別を返し true
    bool GetMark(stirling::FileOffset pos, int* outType) const;
    bool HasMarks() const { return !m_marks.empty(); }

    // --- マーク一覧ダイアログ用の列挙・直接操作（原 CMarkListDlg 経由の編集） ---
    // 位置昇順で列挙
    const std::map<stirling::FileOffset, int>& Marks() const { return m_marks; }
    void SetMark(stirling::FileOffset pos, int type);   // 追加/種別変更（type 0..2）
    void RemoveMark(stirling::FileOffset pos);          // 指定位置のマークを削除（無ければ無処理）

    // ダイナミックマーク（原 StirlingDoc_AdjustMarksAfterEdit FUN_00436e84）:
    //   環境設定 dynamicMark が有効なとき、pos で delCount バイト削除し insCount バイト挿入した
    //   編集に追従してマーク位置を移動する。[pos,pos+delCount) 内のマークは消滅、それ以降は
    //   (insCount-delCount) だけ後方移動。全編集プリミティブ／Undo・Redo から呼ぶ。
    void AdjustMarksForSplice(stirling::FileOffset pos, stirling::FileOffset delCount,
                              stirling::FileOffset insCount);

protected:
    // 逆操作レコード。適用すると当該編集を打ち消す（適用結果が逆方向レコードとなる）。
    struct EditRecord {
        enum Kind { kOverwrite, kInsert, kDelete, kReplace };
        Kind kind = kOverwrite;
        stirling::FileOffset pos = 0;
        stirling::FileOffset count = 0;       // kDelete/kReplace: 削除バイト数
        std::vector<unsigned char> bytes;     // kOverwrite/kInsert/kReplace: 書込む/挿入するバイト列
    };
    // レコードを適用し、それを打ち消す逆レコードを outInv へ返す（Undo/Redo の共通コア）。
    //   退避バッファを確保できない場合は「ドキュメントを一切変更せず」false を返す
    //   （途中まで削除して復元不能になるのを防ぐ）。
    bool ApplyRecord(const EditRecord& r, EditRecord& outInv);

    // --- Undo 履歴の容量管理（Issue #30） ---
    // 1 レコードが退避データとして保持するバイト数。
    static unsigned long long RecordBytes(const EditRecord& r) {
        return static_cast<unsigned long long>(r.bytes.size());
    }
    // 環境設定 undoMemoryLimitMB のバイト換算。0 = 無制限。
    static unsigned long long UndoMemoryLimitBytes();
    // Undo レコードを積む（保持量へ加算する。破棄判定は TrimUndoHistory が行う）。
    void PushUndoRecord(EditRecord&& r);
    // Redo レコードを積む（同上）。
    void PushRedoRecord(EditRecord&& r);
    // 保持合計が上限に収まるまで、Undo スタック先頭（最古の編集）→ Redo スタック先頭
    // （Redo 連鎖の末端）の順に破棄する。破棄した分だけ保存点 m_cleanUndoSize をずらし、
    // 保存点そのものを捨てた場合は到達不能(-1)にする。
    //   各スタックの最後の 1 件は上限を超えていても残す（直近の取り消しは常に可能にする）。
    void TrimUndoHistory();
    // Undo/Redo 履歴を全破棄する（保持量もリセット）。savePointLost=true なら保存点を捨てる。
    void ClearUndoHistory(bool savePointLost);
    // 範囲操作の退避量に対する判断（Issue #30 案B）。
    enum class UndoCapacityDecision {
        kNormal,     // 上限内。従来どおり退避して Undo レコードを積む
        kUndoless,   // 上限超過。利用者が続行を選択＝退避せず実行し履歴を捨てる
        kCancel,     // 上限超過。利用者が中止を選択＝ドキュメントを変更しない
    };
    // 退避量が上限を超えるなら確認ダイアログを出して判断を返す（上限内なら無確認で kNormal）。
    UndoCapacityDecision CheckUndoCapacity(stirling::FileOffset captureBytes) const;
public:
    // この文書に適用する表示設定（拡張子で解決したレコードのコピー。原 view+0x248 相当）。
    const CStirlingSettings& Settings() const { return m_settings; }
    // 現在のパス（GetPathName）から拡張子レコードを再解決して m_settings を確定する。
    //   拡張子別設定の変更後（一覧ダイアログ閉じ）に全文書で呼び直す。
    void ResolveSettings();

protected:
    // オープン時の既定（文字セット/バイトオーダ/挿入モード/編集禁止）を設定から適用する。
    //   fileOpen=true（ファイルを開いた時）のみ「編集禁止」既定を適用（新規文書は編集可のまま）。
    void ApplyOpenDefaults(bool fileOpen);

    // 保存前バックアップ（原 FUN_004340bf）。backupCreate 時、既存ファイルを世代保存する。
    //   命名: dir\name.bak（最新）＋ name.bk1..bk{世代-1}（古い）。name は拡張子込みのファイル名。
    //   backupFolderSpecify 時は backupFolder を dir に用いる。
    void CreateBackup(LPCTSTR path);
    // ディスク上の更新日時を取得する（modeRead|shareDenyNone で開いて GetStatus。原 FUN_00436450）。
    //   開けない場合は false（削除・排他中）。
    static bool ReadDiskTime(LPCTSTR path, CTime& out);

    // 排他制御（原 exclusiveControl）: ドキュメント存続期間中、共有モード付きの参照ハンドルを保持。
    //   0=しない / 1=書込禁止(FILE_SHARE_READ) / 2=読書禁止(共有なし)。保存時は一旦解放して書込む。
    void AcquireLock(LPCTSTR path);
    void ReleaseLock();

    // 変更状態が変わった時に、この文書の全ビューの子フレームタイトルを更新する。
    void RefreshFrameTitles();
    // 前方編集の後処理: Redo破棄（保存点が破棄側なら到達不能化）＋変更フラグ更新。
    void CommitForwardEdit();
    // Undo スタック深さと保存点(m_cleanUndoSize)の比較で変更フラグを更新（Undoで元に戻ると解除）。
    void UpdateModifiedByUndo();

    stirling::BlockList m_blocks;
    CStirlingSettings m_settings;   // この文書の表示設定（拡張子で解決。既定は "*" レコード）
    bool m_overwriteMode = true;   // 原の既定は上書きモード（設定で開時挿入も可）
    int  m_editState = 2;          // 原 doc+0x64: 0=ロック / 1=編集禁止 / 2=編集可（既定=編集可）
    int  m_charset = 1;            // 原 ctor 既定 = 1(シフトJIS)
    bool m_byteOrderBig = false;   // Unicode 表示のバイトオーダ（既定リトル）
    std::vector<EditRecord> m_undoStack;
    std::vector<EditRecord> m_redoStack;
    // m_undoStack + m_redoStack が保持する退避データの合計バイト数（Issue #30）。
    unsigned long long m_undoBytes = 0;
    int m_cleanUndoSize = 0;      // 保存/読込時の Undo 深さ（＝未変更点）。-1=到達不能（保存点消失）
    long m_changeSeq = 0;         // データ変更シーケンス（SetModifiedFlag で単調増加）
    // 位置→マーク種別(0/1/2)。ソート済（std::map）
    std::map<stirling::FileOffset, int> m_marks;
    CTime m_diskTime;             // 最後に読込/保存したときのディスク上の更新日時（原 doc+0x330）
    bool  m_diskTimeValid = false;

    // 排他制御用の存続期間ハンドル（共有モード保持）。RAII で解放漏れを防ぐ（Issue #48）
    stirling::ScopedHandle m_lockHandle;
    CString m_lockPath;                            // ロック中のパス（保存時の再取得に使用）
    DECLARE_MESSAGE_MAP()
};
