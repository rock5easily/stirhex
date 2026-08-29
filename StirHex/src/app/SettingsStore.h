// 設定値のセクション／キー ストアと INI 形式のシリアライズ（Issue #96）。
//
// アプリ設定の保存先をレジストリからファイルへ移すにあたり、CWinApp のプロファイル API
// （GetProfileInt / WriteProfileString 等）が読み書きする実体をこのストアに置き換える。
// 呼び出し側（CAppSettings / CStirlingSettings / MRU / キャレットストア等）は
// プロファイル API 越しに使うため、この単位を直接は知らない。
//
// 形式:
//   - INI 風。`[Section]` 行とその配下の `Key=Value` 行。`;` `#` 始まりの行はコメント
//   - ファイルは UTF-8（BOM なし）。Windows の INI API は使わないため、システム ANSI
//     コードページを一切経由しない（日本語パスが化けない）
//   - 値は原則そのまま書く（`C:\Users\...` がそのまま読める）。制御文字を含む、前後に
//     空白がある、`"` で始まる、のいずれかに当てはまる値だけを引用符で囲んでエスケープする
//   - バイナリ値（キーマップ等）は大文字16進文字列で保持する（MFC の INI モードと同じ流儀）
//
// セクション名・キー名の比較は ASCII 範囲で大文字小文字を区別しない（レジストリの挙動に合わせる）。
//
// この単位は MFC / Win32 に依存しない（単体テストは porting/tests/core_test.cpp）。
#pragma once

#include <string>
#include <vector>

namespace stirling {
namespace settings {

// --- UTF-8 変換（Win32 に依存しない。ASCII 層を ACP 経由にしないため自前で持つ） ---
//   WideToUtf8: 不正なサロゲートは U+FFFD に置き換える（出力は常に妥当な UTF-8）。
std::string WideToUtf8(const std::wstring& text);
//   Utf8ToWide: 不正なシーケンスを検出したら false を返す（out は妥当な範囲まで埋めたうえで
//   置換文字 U+FFFD を含む）。呼び出し側は「壊れた設定ファイル」として扱える。
bool Utf8ToWide(const std::string& text, std::wstring& out);

// --- バイナリ値の16進表現 ---
std::wstring BytesToHex(const unsigned char* data, size_t size);
bool HexToBytes(const std::wstring& text, std::vector<unsigned char>& out);

class SettingsStore {
public:
    struct Entry {
        std::wstring key;
        std::wstring value;
    };
    struct Section {
        std::wstring name;
        std::vector<Entry> entries;
    };

    // 値を探す。無ければ nullptr。
    const std::wstring* Find(const std::wstring& section, const std::wstring& key) const;
    // 値を書く。既存と同じ値なら dirty にしない（無用なファイル書き込みを避ける）。
    void Set(const std::wstring& section, const std::wstring& key, const std::wstring& value);
    // 値を消す。無ければ何もしない（dirty にもしない）。
    void Remove(const std::wstring& section, const std::wstring& key);
    // セクションごと消す（プロファイル API のセクション削除に対応する）。
    void RemoveSection(const std::wstring& section);

    void Clear();
    bool Empty() const { return sections_.empty(); }
    const std::vector<Section>& Sections() const { return sections_; }

    // このストアへ加えた変更の記録（Issue #130）。
    //   複数インスタンスが同じ設定ファイルを使う場合、終了時に自分の全体像で置換すると
    //   別プロセスの更新を古い値で消してしまう。保存側は「最新のファイル内容」へこの
    //   変更だけを適用してから書き戻す。順序を保つため一覧で持つ。
    struct Change {
        enum class Kind { Set, Remove, RemoveSection, Clear };
        Kind kind = Kind::Set;
        std::wstring section;   // Clear では空
        std::wstring key;       // RemoveSection / Clear では空
        std::wstring value;     // Set のみ
    };

    // 変更の有無。ファイルへ書き戻すべきかの判定に使う。
    bool Dirty() const { return dirty_; }
    void ClearDirty() { dirty_ = false; changes_.clear(); }
    // ClearDirty() までに記録した変更（投入順）。
    const std::vector<Change>& Changes() const { return changes_; }
    // 別のストアへ変更を適用する（マージ）。適用側の変更としても記録される。
    void ApplyChanges(const std::vector<Change>& changes);

    // INI テキストへ。セクション・キーは投入順を保つ（差分が読みやすいように）。
    std::wstring Serialize() const;
    // INI テキストから取り込む（既存の同名キーは上書き）。dirty は変えない。
    //   戻り値: 解釈できない行が1つも無ければ true。false でも読めた行は取り込む。
    bool ParseInto(const std::wstring& text);

private:
    std::vector<Section> sections_;
    std::vector<Change> changes_;
    bool dirty_ = false;
    bool recording_ = true;   // ParseInto の間だけ false（読み込みは変更ではない）

    void MarkChanged(const Change& change);

    Section* FindSection(const std::wstring& name);
    const Section* FindSection(const std::wstring& name) const;
};

// 値のエスケープ／復元（Serialize / ParseInto が使う。テストのために公開する）。
std::wstring EncodeValue(const std::wstring& value);
std::wstring DecodeValue(const std::wstring& text);

}  // namespace settings
}  // namespace stirling
