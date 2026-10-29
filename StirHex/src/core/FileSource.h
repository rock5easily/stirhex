// FileSource — 遅延読み込み（Issue #300）で、BlockList のファイル参照ノードが指す元ファイル。
//
// 数GB〜数十GB のファイルを全体読み込みせずに扱うため、BlockList は「元ファイルの区間」を
// 参照するだけのノード（BlockList::IsFileRef）を持てる。その読み出し元がこのクラスで、
//   - 開いたファイルのハンドル（他プロセスの書込を拒否する共有モードで保持。参照ノードが
//     指す内容を変えられないようにするため。これが排他制御のロックも兼ねる）
//   - 64KB ページ単位の小さな読み込みキャッシュ（描画・検索の細かい読みを吸収する）
// を持つ。ページより大きい読み取りはキャッシュを通さず直接読む（保存・比較・チェックサム
// のような逐次の大きな読みでキャッシュを追い出さないため）。
//
// スレッド: 1 つの FileSource は 1 スレッドからだけ使う（BGREP のワーカは自前の BlockList と
//   FileSource を持つ）。
#pragma once

#include "CoreTypes.h"

#include <memory>
#include <string>
#include <vector>

namespace stirling {

class FileSource {
public:
    static constexpr int kPageSize  = 0x10000;   // 64KB
    static constexpr int kPageCount = 256;       // 16MB

    // 開いたハンドル（所有権を受け取る）から作る。shareFlags は Reopen 用に控える。
    //   writable はそのハンドルが書込権を持つか。
    //   型は windows.h をヘッダへ持ち込まないため void*（HANDLE）。
    FileSource(void* handle, std::wstring path, FileOffset size,
               unsigned long shareFlags, bool writable);
    ~FileSource();

    FileSource(const FileSource&) = delete;
    FileSource& operator=(const FileSource&) = delete;

    const std::wstring& Path() const { return path_; }
    FileOffset Size() const { return size_; }
    bool Writable() const { return writable_; }
    bool IsOpen() const { return handle_ != nullptr; }
    void* Handle() const { return handle_; }
    unsigned long ShareFlags() const { return shareFlags_; }
    // 直近の失敗の GetLastError（成功では更新しない）。
    unsigned long LastError() const { return lastError_; }
    // 読み取りに失敗した回数（範囲外を含む。確保の失敗は数えない）。単調に増える。
    //   操作の前後で比べ、失敗が元ファイルの読み取りによるものかを判定する（Issue #306）。
    unsigned long long ReadFailures() const { return readFailures_; }

    // pos を含むページを返す。*outBase にページ先頭の位置、*outLen にページ内の有効長を
    //   格納する。失敗（読取エラー・メモリ不足・ファイル範囲外）は nullptr。
    //   返したポインタは、次にこの FileSource を呼ぶ（ページを追い出しうる）までだけ有効。
    //   保持し続ける呼出側は Epoch() が変わっていないことを確かめてから使う。
    const unsigned char* Page(FileOffset pos, FileOffset* outBase, int* outLen);

    // pos から len バイトを dst へ読む。全バイト読めたときだけ true（範囲外も false）。
    //   ページ以上の長さはキャッシュを通さない。
    bool Read(FileOffset pos, void* dst, size_t len);
    // Read と同じ読み方で、pos から先頭続きに読めたバイト数（0..len）を返す（Issue #326）。
    //   途中で失敗したら、ページ単位（ページ以上の長さは成功した ReadFile 単位）で
    //   そこまでに読めた分を返す。範囲外は読めた分まで（ページ以上の長さは 0）。
    size_t ReadPartial(FileOffset pos, void* dst, size_t len);

    // 上書き保存（Issue #300）用。pos へ len バイト書く。Writable() のときだけ成功しうる。
    //   キャッシュは呼出側が InvalidateCache() で捨てること（書き終えるまで読み出さない前提）。
    bool Write(FileOffset pos, const void* src, size_t len);
    // FlushFileBuffers。遅延書込エラーを検出する（Issue #166 と同じ理由）。
    bool Flush();

    // キャッシュを捨てる（Epoch が進む）。
    void InvalidateCache();
    // ページの追い出し・破棄のたびに進む世代番号。Page() のポインタを保持する側が検証に使う。
    unsigned long long Epoch() const { return epoch_; }

    // ハンドルを閉じる（キャッシュも捨てる）。保存で元ファイルを置き換える直前に使う。
    void Close();
    // 同じパス・共有モードで開き直す。writable なら書込権も要求する（上書き保存の間だけ
    //   書込権付きで開き直し、終わったら読み取り専用へ戻すため）。閉じている間は共有モードの
    //   保護が外れるため、ファイル ID・サイズ・最終更新日時のいずれかが控えと違えば
    //   失敗扱いにする（同じサイズの別内容へ置き換えられていても参照ノードを読ませない）。
    //   失敗時は閉じたまま。
    bool Reopen(bool writable);
    // 自分で書き込んだ後（上書き保存）に、最終更新日時を現在時刻へ設定して控え直す。
    //   日時は書込やクローズの時点で遅れて更新されうるため、明示的に確定させてから控える。
    bool StampAfterWrite();

private:
    struct Slot {
        std::unique_ptr<unsigned char[]> data;
        FileOffset base = -1;              // ページ先頭の位置（-1=空き）
        int len = 0;
        unsigned long long lastUse = 0;
    };
    // *outDone（省略可）には、失敗時も含めて dst の先頭へ読めたバイト数を格納する。
    bool ReadDirect(FileOffset pos, void* dst, size_t len, size_t* outDone = nullptr);
    // ReadDirect の本体（失敗を数えない）
    bool ReadDirectOnce(FileOffset pos, void* dst, size_t len, size_t* outDone);

    // 開いたファイルの同一性の控え（Reopen で照合する）。
    struct Identity {
        unsigned long volume = 0;
        unsigned long indexHigh = 0;
        unsigned long indexLow = 0;
        unsigned long long lastWrite = 0;
        bool valid = false;
    };
    static Identity IdentityOf(void* handle);

    void*         handle_ = nullptr;
    std::wstring  path_;
    FileOffset    size_ = 0;
    unsigned long shareFlags_ = 0;
    bool          writable_ = false;
    unsigned long lastError_ = 0;
    unsigned long long readFailures_ = 0;
    Identity      identity_;
    std::vector<Slot> slots_;
    unsigned long long useClock_ = 0;
    unsigned long long epoch_ = 1;
};

}  // namespace stirling
