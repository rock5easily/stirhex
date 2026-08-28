// StructDef — 構造体編集バー用の Struct.def パーサと型モデル（原 StructDef_Parse FUN_00461eb3 群相当）。
//   Struct.def は C 風構造体定義:
//     struct NAME { TYPE field; TYPE field[N]; TYPE field[N][M]; NESTED sub; ... };
//   組込型は原の正確な集合のみ（別名・unsigned 前置は非対応。原では未知＝構造体名扱い）:
//     char/CHAR byte BYTE short/SHORT word WORD long/LONG dword DWORD float/FLOAT double/DOUBLE。
//   大小文字で表示基数が変わる（byte=符号なし/BYTE=16進 等。§2/§6 の型表を参照）。
//   ネスト構造体（前方参照不可）・1〜2次元配列に対応。構造体編集バー用に、指定位置の
//   バイト列を各フィールドの型・表示基数・バイトオーダーに従って解釈し、編集可能なツリーを構築する。
//   詳細: analysis_artifacts/docs/18_struct_edit.md §2/§3/§6。
#pragma once

#include <string>
#include <vector>

namespace stirling {

// フィールドの基本種別（= 原 typeCode。member+4）。値解釈の幅・符号の分岐に用いる。
//   char/byte は typeCode 0/1、short/word=2/3、long/dword=4/5、float=6、double=7、struct=8。
enum class FieldKind {
    Char,    // typeCode 0: 1B 符号付き読み取り
    Byte,    // typeCode 1: 1B 符号なし読み取り（byte/BYTE 共通、基数のみ相違）
    Short,   // typeCode 2: 2B（u16 読み取り）
    Word,    // typeCode 3: 2B（u16 読み取り、short と同幅・基数のみ相違）
    Long,    // typeCode 4: 4B（u32 読み取り、"%d" で符号付き表示）
    Dword,   // typeCode 5: 4B（u32 読み取り）
    Float,   // typeCode 6: 4B 浮動小数
    Double,  // typeCode 7: 8B 浮動小数
    Struct,  // typeCode 8: ネスト構造体（structIndex 参照）
    Unknown, // 未知型（サイズ 0、値は "?"）
};

// 表示基数（= 原 member+0）。0=符号付き10進 / 1=（1・2byte）10進・（4byte）符号なし10進 /
//   2=16進 "0x%0NX" / -1=浮動小数（float/double）。右クリックで 0/1/2 を上書き可（原 this+0x250）。
enum : int { kRadixSignedDec = 0, kRadixDec1 = 1, kRadixHex = 2, kRadixFloat = -1 };

struct StructField {
    std::string typeName;      // 元の型トークン（表示用）
    std::string name;          // フィールド名
    int         arrayCount = 1;   // 配列 1 次元目の要素数（非配列は 1）
    int         arrayCount2 = 1;  // 配列 2 次元目の要素数（[N][M] の M。無ければ 1）
    FieldKind   kind = FieldKind::Unknown;
    int         radix = kRadixSignedDec;  // 既定表示基数（型キーワードの大小文字で決まる）
    int         structIndex = -1;  // kind==Struct のとき定義集合内のインデックス
};

struct StructDef {
    std::string name;
    std::vector<StructField> fields;
};

// 解釈済みのツリーノード（原 DDS2 ツリーグリッドの 1 ノード）。
//   ネスト構造体・配列は hasChildren=true の展開可能ノードとなる。
struct StructNode {
    std::string type;    // 型名（型列。配列要素の合成ノード "[i]" は空）
    std::string name;    // シンボル名（配列は "field[N]"、要素は "[i]"）
    // 値（葉/char配列コンテナ。展開のみのコンテナは空）。
    //   [wide層] 表示に直接使う。char 配列は UTF-8 で CP932 外の文字も持つため
    //   ワイドで保持する（Issue #107）。数値は ASCII 数字なのでワイド化は無損失。
    std::wstring value;
    bool hasChildren = false;
    std::vector<StructNode> children;

    // --- 編集用（スカラ葉のみ editable=true。書き戻しに使用） ---
    bool      editable = false;   // 値を編集してデータへ書き戻せるか
    int       offset = -1;        // struct base からの相対バイトオフセット
    int       size = 0;           // バイトサイズ
    FieldKind kind = FieldKind::Unknown;  // 値解釈の型（typeCode）
    int       radix = kRadixSignedDec;    // 表示基数（右クリックで上書き。既定はフィールド基数）
};

// bytes（size バイト）を kind・表示基数 radix・バイトオーダ big に従い値文字列へ整形する
//   純粋関数（原 StructValue_ReadScalar+FormatNumber, §6 実測表）。右クリック基数変更でも使用。
std::string FormatScalarValue(FieldKind kind, int size, const unsigned char* bytes,
                              bool big, int radix);

// FormatScalarValue のワイド版（StructNode::value へ入れる形）。数値表記は ASCII の
//   ため、1 文字ずつ広げるだけで内容は変わらない。
std::wstring FormatScalarValueW(FieldKind kind, int size, const unsigned char* bytes,
                                bool big, int radix);

// text を kind の型（size バイト）として解釈し、big エンディアンでバイト列へ符号化する
//   純粋関数（原 StructValue_ParseString/ParseInt/ParseFloat, §6 に忠実）。
//   整数: 先頭 "0x"/"0X"→16進（以降 [0-9A-Fa-f]、符号なし）/ 先頭 '-'→負の10進 / それ以外→10進。
//     "0" 単独=0。先頭 '0' の後に 'x' 以外が続く形は不正（原の仕様）。前後空白も不正。
//     幅レンジ検証（原 ParseInt: 上位32bit<1 かつ 下位が幅内 or 符号ビット）で範囲外は false。
//   float/double: 符号・小数点1個・指数（e/E/d/D の直後に必ず符号）を検証して strtod、全消費チェック。
//   成功で true（out に size バイト）。構文/範囲エラーは false。
bool EncodeScalar(FieldKind kind, int size, const std::string& text, bool big,
                  std::vector<unsigned char>& out);

class StructDefSet {
public:
    // Struct.def をパースする。成功で true。err に失敗理由（任意）を返す。
    //   [byte層境界] err は表示用のワイド文字列。def 由来のトークン（CP932 バイト列）は
    //   ここでワイドへ変換して埋め込むため、呼び出し側はそのまま UI へ渡せる。
    bool ParseFile(const wchar_t* path, std::wstring* err);
    bool ParseText(const std::string& text, std::wstring* err);

    const std::vector<StructDef>& Defs() const { return m_defs; }
    bool Empty() const { return m_defs.empty(); }
    int  FindByName(const std::string& n) const;

    // 構造体 defIndex の総バイトサイズ（ネスト・配列含む）。範囲外は 0。
    int  SizeOfStruct(int defIndex) const;

    // 構造体 defIndex を data（先頭=キャレット位置）で解釈し、ツリーを構築する。
    //   root.children にトップレベルフィールドが入る。big=true でビッグエンディアン。
    //   charset は char/byte 配列の文字列化に使用（ASCII/SJIS/EUC-JP/Unicode/EBCDIC/EBCIDK）。
    //   不足範囲は "----"。
    //   radixOverride>=0 のとき全スカラ葉をその基数で表示（原 this+0x250 の全体上書き。
    //   -1=型ごとの既定基数）。float/double は基数に依らず浮動小数表示。
    void BuildTree(int defIndex, const std::vector<unsigned char>& data,
                   bool big, int charset, StructNode& root, int radixOverride = -1) const;

private:
    // 単一フィールドの 1 要素サイズ（配列を除く）。
    int  ElemSize(const StructField& f) const;
    // フィールドの総サイズ（要素サイズ×要素数）。
    int  FieldSize(const StructField& f) const;
    // 再帰でノードを生成（baseOff は data 内オフセット、out は親の children）。
    void EmitNodes(int defIndex, const std::vector<unsigned char>& data, int baseOff,
                   bool big, int charset, int radixOverride, std::vector<StructNode>& out) const;
    // 単一要素（スカラ or 単体ネスト構造体）1 個分のノードを out へ追加。
    void EmitElement(const StructField& f, const std::vector<unsigned char>& data, int off,
                     bool big, int charset, int radixOverride, const std::string& type,
                     const std::string& name, std::vector<StructNode>& out) const;
    // 1 次元ぶんの要素列（count 個）を container.children へ生成（char/byte は container.value も設定）。
    void EmitRow(const StructField& f, const std::vector<unsigned char>& data, int off, int count,
                 bool big, int charset, int radixOverride, StructNode& container) const;

    // 組込型トークンの解決結果（原の型表 §2）。found=false は非組込（＝構造体名候補）。
    struct Builtin { bool found; FieldKind kind; int radix; int size; };
    // 組込型トークンを (kind,radix,size) へ解決（原 StructDef_Parse の lstrcmpA カスケード）。
    static Builtin ResolveBuiltin(const std::string& type);

    std::vector<StructDef> m_defs;
};

}  // namespace stirling
