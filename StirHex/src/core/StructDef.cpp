// StructDef 実装。Struct.def の字句解析→構文解析→型解決、および値解釈。
#include "core/StructDef.h"
#include "core/CharConv.h"
#include "core/Cp932Text.h"   // WideFromCp932（def 由来トークンを表示用ワイドへ）
#include "util/ScopedHandle.h"

#include <windows.h>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace stirling {

namespace {

// ---- 字句解析 ----------------------------------------------------------
// トークン種別: 識別子/数値（連続する [A-Za-z0-9_]）、単一記号 { } ; [ ]。
// 空白と C/C++ コメント（// と /* */）を読み飛ばす。
struct Token {
    enum Kind { kIdent, kSymbol, kEnd } kind;
    std::string text;   // kIdent: 語, kSymbol: 1文字
};

class Lexer {
public:
    explicit Lexer(const std::string& s) : m_s(s), m_i(0) {}

    Token Next() {
        SkipTrivia();
        if (m_i >= m_s.size()) return { Token::kEnd, "" };
        const char c = m_s[m_i];
        if (IsWord(c)) {
            size_t start = m_i;
            while (m_i < m_s.size() && IsWord(m_s[m_i])) ++m_i;
            return { Token::kIdent, m_s.substr(start, m_i - start) };
        }
        // 単一記号
        ++m_i;
        return { Token::kSymbol, std::string(1, c) };
    }

private:
    static bool IsWord(char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') || c == '_';
    }
    void SkipTrivia() {
        for (;;) {
            while (m_i < m_s.size() &&
                   (m_s[m_i] == ' ' || m_s[m_i] == '\t' || m_s[m_i] == '\r' ||
                    m_s[m_i] == '\n' || m_s[m_i] == '\f' || m_s[m_i] == '\v')) {
                ++m_i;
            }
            if (m_i + 1 < m_s.size() && m_s[m_i] == '/' && m_s[m_i + 1] == '/') {
                m_i += 2;
                while (m_i < m_s.size() && m_s[m_i] != '\n') ++m_i;
                continue;
            }
            if (m_i + 1 < m_s.size() && m_s[m_i] == '/' && m_s[m_i + 1] == '*') {
                m_i += 2;
                while (m_i + 1 < m_s.size() && !(m_s[m_i] == '*' && m_s[m_i + 1] == '/')) ++m_i;
                if (m_i + 1 < m_s.size()) m_i += 2;
                continue;
            }
            break;
        }
    }
    const std::string& m_s;
    size_t m_i;
};

// 配列要素数の上限。要素数は FieldSize / EmitRow で int の乗算とノード生成に使われるため、
//   INT_MAX まで許すと符号付き整数オーバーフローと事実上のハングを招く。
//   1 メンバーあたり 65536 要素あれば構造体編集の実用範囲を十分に覆う（原の struct.def も
//   最大 260 要素）。2 次元配列は要素数の積にも同じ上限を課す。
const long kMaxArrayCount = 65536;

// 配列要素数のパース。10進の正整数のみを受け付け、末尾の余分な文字・範囲外・0 以下は
//   すべて失敗として扱う（atoi は変換失敗と 0 を区別できず、オーバーフローが未定義動作）。
//   字句解析器は符号を語に含めないため、ここに来るのは [0-9A-Za-z_]+ に限られる。
bool ParseArrayCount(const std::string& text, int* out) {
    if (text.empty()) { return false; }
    errno = 0;
    char* end = nullptr;
    const long v = std::strtol(text.c_str(), &end, 10);
    if (end != text.c_str() + text.size()) { return false; }   // 数値以外が混じる（例: 10abc）
    if (errno == ERANGE || v <= 0 || v > kMaxArrayCount) { return false; }
    *out = static_cast<int>(v);
    return true;
}

// 整数を data からバイトオーダーに従って組み立て（符号なし raw を返す）。
unsigned long long AssembleRaw(const unsigned char* p, int size, bool big) {
    unsigned long long v = 0;
    for (int i = 0; i < size; ++i) {
        const unsigned char b = big ? p[i] : p[size - 1 - i];
        v = (v << 8) | b;
    }
    return v;
}

}  // namespace

StructDefSet::Builtin StructDefSet::ResolveBuiltin(const std::string& type) {
    // 原 StructDef_Parse(FUN_00461eb3) の型キーワード表（§2）。大小文字で表示基数が変わる。
    //   (kind=typeCode, radix=member+0, size)。別名や unsigned 前置は原に無く、非組込＝構造体名扱い。
    struct Row { const char* kw; FieldKind kind; int radix; int size; };
    static const Row kTable[] = {
        {"char",   FieldKind::Char,   kRadixSignedDec, 1}, {"CHAR",   FieldKind::Char,   kRadixSignedDec, 1},
        {"byte",   FieldKind::Byte,   kRadixDec1,      1}, {"BYTE",   FieldKind::Byte,   kRadixHex,       1},
        {"short",  FieldKind::Short,  kRadixSignedDec, 2}, {"SHORT",  FieldKind::Short,  kRadixSignedDec, 2},
        {"word",   FieldKind::Word,   kRadixDec1,      2}, {"WORD",   FieldKind::Word,   kRadixHex,       2},
        {"long",   FieldKind::Long,   kRadixSignedDec, 4}, {"LONG",   FieldKind::Long,   kRadixSignedDec, 4},
        {"dword",  FieldKind::Dword,  kRadixDec1,      4}, {"DWORD",  FieldKind::Dword,  kRadixHex,       4},
        {"float",  FieldKind::Float,  kRadixFloat,     4}, {"FLOAT",  FieldKind::Float,  kRadixFloat,     4},
        {"double", FieldKind::Double, kRadixFloat,     8}, {"DOUBLE", FieldKind::Double, kRadixFloat,     8},
    };
    for (const auto& r : kTable) {
        if (type == r.kw) return { true, r.kind, r.radix, r.size };
    }
    return { false, FieldKind::Unknown, kRadixSignedDec, 0 };
}

bool StructDefSet::ParseFile(const wchar_t* path, std::wstring* err) {
    ScopedHandle h(::CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h.Valid()) {
        if (err) *err = L"Struct.def を開けません";
        return false;
    }
    std::string text;
    char buf[4096];
    DWORD read = 0;
    while (::ReadFile(h.Get(), buf, sizeof(buf), &read, nullptr) && read > 0) {
        text.append(buf, read);
    }
    h.Close();
    return ParseText(text, err);
}

bool StructDefSet::ParseText(const std::string& text, std::wstring* err) {
    m_defs.clear();
    Lexer lex(text);
    Token t = lex.Next();
    while (t.kind != Token::kEnd) {
        // "struct" NAME "{" fields "}" ";"
        if (t.kind == Token::kIdent && t.text == "struct") {
            Token nameTok = lex.Next();
            if (nameTok.kind != Token::kIdent) { if (err) *err = L"struct 名が不正"; return !m_defs.empty(); }
            Token brace = lex.Next();
            if (!(brace.kind == Token::kSymbol && brace.text == "{")) {
                if (err) *err = L"'{' が必要";
                return !m_defs.empty();
            }
            StructDef def;
            def.name = nameTok.text;
            for (;;) {
                Token ft = lex.Next();
                if (ft.kind == Token::kEnd) break;
                if (ft.kind == Token::kSymbol && ft.text == "}") break;
                if (ft.kind != Token::kIdent) continue;   // 予期しない記号は読み飛ばし
                // 型トークン。'struct FOO x;' の 'struct' 前置のみ畳む（原の struct キーワード対応）。
                std::string typeTok = ft.text;
                if (typeTok == "struct") {
                    Token nx = lex.Next();
                    if (nx.kind == Token::kIdent) typeTok = nx.text;
                }
                StructField fld;
                fld.typeName = typeTok;
                // フィールド名
                Token fnameTok = lex.Next();
                if (!(fnameTok.kind == Token::kIdent)) {
                    // 名前が来ない（記号 ; 等）→スキップ
                    continue;
                }
                fld.name = fnameTok.text;
                // 配列 "[" NUMBER "]" を最大 2 次元まで（原 [N] / [N][M]）。
                Token nx = lex.Next();
                for (int dim = 0; dim < 2 && nx.kind == Token::kSymbol && nx.text == "["; ++dim) {
                    Token num = lex.Next();
                    int cnt = 0;
                    if (num.kind != Token::kIdent || !ParseArrayCount(num.text, &cnt)) {
                        // 原は atoi で黙って 1 要素として扱っていたが、誤解釈を避けるため
                        //   未定義の型と同じくパースエラーとして中断する。
                        if (err) {
                            *err = L"配列の要素数が不正です: " + WideFromCp932(fld.name.c_str()) +
                                   L"[" + WideFromCp932(num.text.c_str()) + L"]";
                        }
                        m_defs.push_back(def);
                        return false;
                    }
                    // 2 次元配列は要素数の積にも上限を課す（FieldSize は c1*c2 を int で掛ける）。
                    if (dim == 1 && static_cast<long long>(fld.arrayCount) * cnt > kMaxArrayCount) {
                        if (err) {
                            *err = L"配列の要素数が大きすぎます: " + WideFromCp932(fld.name.c_str());
                        }
                        m_defs.push_back(def);
                        return false;
                    }
                    if (dim == 0) fld.arrayCount = cnt; else fld.arrayCount2 = cnt;
                    Token close = lex.Next();   // "]"
                    (void)close;
                    nx = lex.Next();            // 次の "[" か ";"
                }
                // nx はここで ";" のはず（違っても次ループで回復）
                // 型解決（原の型表 → 非組込は既定義の構造体のみ。前方参照不可）。
                const Builtin b = ResolveBuiltin(fld.typeName);
                if (b.found) {
                    fld.kind = b.kind;
                    fld.radix = b.radix;
                } else {
                    int si = FindByName(fld.typeName);
                    if (si >= 0) {
                        fld.kind = FieldKind::Struct; fld.structIndex = si; fld.radix = kRadixSignedDec;
                    } else {
                        // 原 0x24「未定義構造体」相当。前方参照は許さずエラーで中断。
                        if (err) *err = L"未定義の型または構造体です: " + WideFromCp932(fld.typeName.c_str());
                        m_defs.push_back(def);
                        return false;
                    }
                }
                def.fields.push_back(fld);
            }
            m_defs.push_back(def);
            t = lex.Next();
            if (t.kind == Token::kSymbol && t.text == ";") t = lex.Next();  // 末尾 ';'
            continue;
        }
        t = lex.Next();
    }
    return !m_defs.empty();
}

int StructDefSet::FindByName(const std::string& n) const {
    for (size_t i = 0; i < m_defs.size(); ++i) {
        if (m_defs[i].name == n) return static_cast<int>(i);
    }
    return -1;
}

int StructDefSet::ElemSize(const StructField& f) const {
    switch (f.kind) {
    case FieldKind::Char: case FieldKind::Byte:   return 1;
    case FieldKind::Short: case FieldKind::Word:  return 2;
    case FieldKind::Long: case FieldKind::Dword:  return 4;
    case FieldKind::Float:                        return 4;
    case FieldKind::Double:                       return 8;
    case FieldKind::Struct:                       return SizeOfStruct(f.structIndex);
    default:                                       return 0;
    }
}

int StructDefSet::FieldSize(const StructField& f) const {
    const int c1 = (f.arrayCount  > 0 ? f.arrayCount  : 1);
    const int c2 = (f.arrayCount2 > 0 ? f.arrayCount2 : 1);
    return ElemSize(f) * c1 * c2;
}

int StructDefSet::SizeOfStruct(int defIndex) const {
    if (defIndex < 0 || defIndex >= static_cast<int>(m_defs.size())) return 0;
    int total = 0;
    for (const auto& f : m_defs[defIndex].fields) total += FieldSize(f);
    return total;
}

namespace {

// 整数フィールドの値文字列（原 StructValue_ReadScalar+FormatNumber, §6 実測表に一致）。
//   読み取り: char(kind Char)のみ符号拡張、他は幅ぶんゼロ拡張の符号なし。
//   書式(radix): 2=16進 "0x%0{2*size}X" / 0=符号付き "%d"（4byte は 32bit 再解釈で符号付き） /
//   1=（1・2byte）"%d" 正の値 ・（4byte）符号なし "%u"。
std::string FormatInt(const unsigned char* p, FieldKind kind, int size, bool big, int radix) {
    const unsigned long long raw = AssembleRaw(p, size, big);
    const unsigned long long widthMask = (size >= 8) ? ~0ull : ((1ull << (size * 8)) - 1);
    const unsigned long long uw = raw & widthMask;
    char buf[48];
    if (radix == kRadixHex) {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "0x%0*llX", size * 2, uw);
        return buf;
    }
    if (radix == kRadixDec1 && size == 4) {
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%u", static_cast<unsigned int>(uw));
        return buf;
    }
    // radix0（符号付き）または radix1 の 1・2byte。原の読み取り符号に従い符号付き値を作る:
    //   char=8bit 符号拡張 / long(4byte,radix0)=32bit 符号付き再解釈 / それ以外は符号なし幅値。
    long long sval;
    if (kind == FieldKind::Char) {
        sval = static_cast<signed char>(static_cast<unsigned char>(uw));
    } else if (size == 4) {
        sval = static_cast<long long>(static_cast<int>(static_cast<unsigned int>(uw)));
    } else {
        sval = static_cast<long long>(uw);   // byte/short/word: 0..width（正）
    }
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%lld", sval);
    return buf;
}

std::string FormatFloat(const unsigned char* p, int size, bool big) {
    unsigned char tmp[8] = {0};   // size<8 でも末尾を未初期化のまま読まない（C6001）
    for (int i = 0; i < size && i < 8; ++i) tmp[i] = big ? p[size - 1 - i] : p[i];
    char buf[64];
    if (size == 4) {
        float f; memcpy(&f, tmp, 4);
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%g", static_cast<double>(f));
    } else {
        double d; memcpy(&d, tmp, 8);
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%g", d);
    }
    return buf;
}

// "[i]" のインデックス名を生成。
std::string IndexName(int i) {
    char b[24]; _snprintf_s(b, sizeof(b), _TRUNCATE, "[%d]", i);
    return b;
}

}  // namespace

// FormatScalarValue のワイド版（StructNode::value 用）。
//   数値表記は ASCII なので 1 文字ずつ広げるだけ。書式そのものは narrow 版が唯一の実装。
std::wstring FormatScalarValueW(FieldKind kind, int size, const unsigned char* bytes,
                                bool big, int radix) {
    const std::string s = FormatScalarValue(kind, size, bytes, big, radix);
    return std::wstring(s.begin(), s.end());
}

// スカラ型 1 要素の値文字列（公開純粋関数。型 kind・幅 size・表示基数 radix に従う。§6 実測表）。
std::string FormatScalarValue(FieldKind kind, int size, const unsigned char* bytes,
                              bool big, int radix) {
    switch (kind) {
    case FieldKind::Char:
    case FieldKind::Byte:
    case FieldKind::Short:
    case FieldKind::Word:
    case FieldKind::Long:
    case FieldKind::Dword:  return FormatInt(bytes, kind, size, big, radix);
    case FieldKind::Float:  return FormatFloat(bytes, 4, big);
    case FieldKind::Double: return FormatFloat(bytes, 8, big);
    default:                return "?";
    }
}

namespace {
// 配列名 "base[c]" / "base[c1][c2]"。
std::string ArrayName2(const std::string& base, int c1, int c2) {
    char b[48];
    if (c2 > 1) _snprintf_s(b, sizeof(b), _TRUNCATE, "[%d][%d]", c1, c2);
    else        _snprintf_s(b, sizeof(b), _TRUNCATE, "[%d]", c1);
    return base + b;
}

// 原 StructRow_BuildColumns は組込型を文字列リソース6002..6009の標準表記で表示する。
std::string DisplayTypeName(const StructField& field) {
    switch (field.kind) {
    case FieldKind::Char:   return "char";
    case FieldKind::Byte:   return "BYTE";
    case FieldKind::Short:  return "short";
    case FieldKind::Word:   return "WORD";
    case FieldKind::Long:   return "long";
    case FieldKind::Dword:  return "DWORD";
    case FieldKind::Float:  return "float";
    case FieldKind::Double: return "double";
    default:                return field.typeName;
    }
}
}  // namespace

// 単一要素（スカラ or ネスト構造体）1 個分のノードを生成して out へ追加。
void StructDefSet::EmitElement(const StructField& f, const std::vector<unsigned char>& data,
                               int off, bool big, int charset, int radixOverride,
                               const std::string& type, const std::string& name,
                               std::vector<StructNode>& out) const {
    const int dataLen = static_cast<int>(data.size());
    const int esize = ElemSize(f);
    if (f.kind == FieldKind::Struct) {
        StructNode n;
        n.type = type; n.name = name; n.hasChildren = true;
        EmitNodes(f.structIndex, data, off, big, charset, radixOverride, n.children);
        out.push_back(std::move(n));
        return;
    }
    // 全体基数上書き（原 this+0x250）。float/double は FormatScalarValue 内で基数を無視。
    const int useRadix = (radixOverride >= 0) ? radixOverride : f.radix;
    StructNode n;
    n.type = type; n.name = name; n.hasChildren = false;
    const bool inRange = (off + esize <= dataLen);
    n.value = inRange ? FormatScalarValueW(f.kind, esize, &data[off], big, useRadix) : L"----";
    n.editable = inRange; n.offset = off; n.size = esize; n.kind = f.kind; n.radix = useRadix;
    out.push_back(std::move(n));
}

// 1 次元ぶんの要素列（count 個, elemStride バイト間隔）を container.children へ生成。
//   char/byte のスカラ配列は container.value に文字列表現も設定する。
void StructDefSet::EmitRow(const StructField& f, const std::vector<unsigned char>& data,
                           int off, int count, bool big, int charset, int radixOverride,
                           StructNode& container) const {
    const int dataLen = static_cast<int>(data.size());
    const int esize = ElemSize(f);
    if ((f.kind == FieldKind::Char || f.kind == FieldKind::Byte)) {
        // charset 別に文字列化（原 FUN_0045fecb。先頭 256 バイト上限は内部で処理）。
        container.value = (off + count <= dataLen)
            ? FormatStructCharArrayW(charset, &data[off], count) : L"----";
    }
    for (int i = 0; i < count; ++i) {
        EmitElement(f, data, off + i * esize, big, charset, radixOverride, "", IndexName(i),
                    container.children);
    }
}

void StructDefSet::EmitNodes(int defIndex, const std::vector<unsigned char>& data, int baseOff,
                             bool big, int charset, int radixOverride,
                             std::vector<StructNode>& out) const {
    if (defIndex < 0 || defIndex >= static_cast<int>(m_defs.size())) return;
    int off = baseOff;
    for (const auto& f : m_defs[defIndex].fields) {
        const int esize = ElemSize(f);
        const int c1 = (f.arrayCount  > 0 ? f.arrayCount  : 1);
        const int c2 = (f.arrayCount2 > 0 ? f.arrayCount2 : 1);
        const std::string displayType = DisplayTypeName(f);

        if (f.kind == FieldKind::Unknown || esize == 0) {
            StructNode n;
            n.type = displayType + ((c1 > 1 || c2 > 1) ? "[]" : "");
            n.name = f.name; n.value = L"?"; n.hasChildren = false;
            out.push_back(std::move(n));
            off += esize * c1 * c2;
            continue;
        }

        if (c1 <= 1 && c2 <= 1) {
            // スカラ or 単体ネスト構造体。
            EmitElement(f, data, off, big, charset, radixOverride, displayType, f.name, out);
        } else if (c2 <= 1) {
            // 1 次元配列: コンテナ "name[c1]" → 各要素 "[i]"。
            StructNode arr;
            arr.type = displayType; arr.name = ArrayName2(f.name, c1, 1); arr.hasChildren = true;
            EmitRow(f, data, off, c1, big, charset, radixOverride, arr);
            out.push_back(std::move(arr));
        } else {
            // 2 次元配列: コンテナ "name[c1][c2]" → 各行 "[i]"（1 次元コンテナ）→ 要素 "[j]"。
            StructNode arr;
            arr.type = displayType; arr.name = ArrayName2(f.name, c1, c2); arr.hasChildren = true;
            for (int i = 0; i < c1; ++i) {
                StructNode row;
                row.type = ""; row.name = IndexName(i); row.hasChildren = true;
                EmitRow(f, data, off + i * esize * c2, c2, big, charset, radixOverride, row);
                arr.children.push_back(std::move(row));
            }
            out.push_back(std::move(arr));
        }
        off += esize * c1 * c2;
    }
}

void StructDefSet::BuildTree(int defIndex, const std::vector<unsigned char>& data,
                             bool big, int charset, StructNode& root, int radixOverride) const {
    root = StructNode();
    root.hasChildren = true;
    EmitNodes(defIndex, data, 0, big, charset, radixOverride, root.children);
}

namespace {
// 整数の下位 size バイトを big/little で書き出す（原 StructValue_WriteScalar と同じ幅切り出し）。
void PutInt(unsigned long long v, int size, bool big, std::vector<unsigned char>& out) {
    out.resize(size);
    for (int i = 0; i < size; ++i) {
        const int sh = big ? (8 * (size - 1 - i)) : (8 * i);
        out[i] = static_cast<unsigned char>((v >> sh) & 0xff);
    }
}

inline bool IsDigit(char c)    { return c >= '0' && c <= '9'; }
inline bool IsHexDigit(char c) {
    return IsDigit(c) || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

// 原 StructValue_ParseInt(FUN_0043c13b): 64bit 累算 + 幅レンジ検証。
//   isHex 時は s[2..] を16進、そうでなければ先頭 '-' 付き10進として v を作る。
//   範囲判定は原と同一（上位32bit と下位32bit を用いる）。成功で out へ下位 size バイト。
bool ParseIntFaithful(const std::string& s, int size, bool isHex, bool big,
                      std::vector<unsigned char>& out) {
    unsigned long long v = 0;
    if (!isHex) {
        const bool neg = (!s.empty() && s[0] == '-');
        for (size_t i = neg ? 1 : 0; i < s.size(); ++i) {
            v = v * 10u + static_cast<unsigned>(s[i] - '0');
        }
        if (neg) v = static_cast<unsigned long long>(-static_cast<long long>(v));
    } else {
        for (size_t i = 2; i < s.size(); ++i) {
            const char c = s[i];
            const unsigned d = IsDigit(c) ? (c - '0') : ((c & 0x5f) - 'A' + 10);
            v = v * 16u + d;
        }
    }
    const int hi = static_cast<int>(static_cast<unsigned int>(v >> 32));
    const int lo = static_cast<int>(static_cast<unsigned int>(v));
    const unsigned int ulo = static_cast<unsigned int>(v);
    bool ok;
    if (size == 1)      ok = (hi < 1) && (lo < 0 || ulo < 0x100u);
    else if (size == 2) ok = (hi < 1) && (lo < 0 || ulo < 0x10000u);
    else                ok = (hi < 1);   // 4byte（原 (hi<2)&&(hi<1) = hi<1）
    if (!ok) return false;
    PutInt(v, size, big, out);
    return true;
}

// 原 StructValue_ParseFloat(FUN_0043bf11) の書式検証: 符号・小数点1個・指数(e/E/d/D の直後に必ず符号)。
bool ValidateFloatFormat(const std::string& s) {
    const size_t n = s.size();
    if (n == 0) return false;
    size_t i = 0;
    const bool sign = (s[0] == '+' || s[0] == '-');
    if (sign) i = 1;
    if (sign && n == 1) return false;
    bool dot = false, expo = false;
    for (; i < n; ++i) {
        const char c = s[i];
        if (IsDigit(c)) continue;
        if (c == '.') {
            if (dot || expo) return false;   // 小数点は指数より前・1個のみ
            dot = true;
        } else if (c == 'e' || c == 'E' || c == 'd' || c == 'D') {
            if (expo) return false;
            ++i;                              // 原は指数の直後に必ず符号を要求
            if (i >= n) return false;
            if (s[i] != '+' && s[i] != '-') return false;
            expo = true;
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace

bool EncodeScalar(FieldKind kind, int size, const std::string& text, bool big,
                  std::vector<unsigned char>& out) {
    const std::string& s = text;   // 原は前後空白を許さない（トリムしない）
    if (s.empty()) return false;

    if (kind == FieldKind::Float || kind == FieldKind::Double) {
        if (!ValidateFloatFormat(s)) return false;
        char* end = nullptr;
        const double d = strtod(s.c_str(), &end);
        if (end != s.c_str() + s.size()) return false;   // 全消費チェック
        if (size == 4) {
            float f = static_cast<float>(d);
            unsigned char tmp[4]; memcpy(tmp, &f, 4);
            out.resize(4);
            for (int i = 0; i < 4; ++i) out[i] = big ? tmp[3 - i] : tmp[i];
        } else {
            unsigned char tmp[8]; memcpy(tmp, &d, 8);
            out.resize(8);
            for (int i = 0; i < 8; ++i) out[i] = big ? tmp[7 - i] : tmp[i];
        }
        return true;
    }

    // 整数（char/byte/short/word/long/dword）。原 StructValue_ParseString の判定。
    bool isHex;
    if (s[0] == '0') {
        if (s.size() < 2) { PutInt(0, size, big, out); return true; }   // "0" → 0
        if ((s[1] | 0x20) != 'x') return false;                        // "0"+非x は不正
        isHex = true;
        for (size_t i = 2; i < s.size(); ++i) if (!IsHexDigit(s[i])) return false;
        if (s.size() == 2) return false;                                // "0x" のみは不正
    } else {
        isHex = false;
        const size_t start = (s[0] == '-') ? 1 : 0;
        for (size_t i = start; i < s.size(); ++i) if (!IsDigit(s[i])) return false;
        // 注: 原は "-" 単独も検証通過→値 0（ParseIntFaithful が 0 を返す）。忠実にそのまま許容。
    }
    return ParseIntFaithful(s, size, isHex, big, out);
}

}  // namespace stirling
