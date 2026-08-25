// GDI オブジェクトの RAII ラッパ（Issue #48 / 親 #16）。
//   描画は再描画のたびに走るため、生成したフォント／ペン等の DeleteObject 漏れや
//   DC への選択の復帰漏れは GDI オブジェクト枯渇に直結する。生成＝所有、選択＝復帰を
//   スコープで担保する。
#pragma once

#include <afxwin.h>
#include <windows.h>

namespace stirling {

// 生成した GDI オブジェクト（HFONT/HPEN/HBRUSH/HBITMAP 等）を所有し、破棄時に DeleteObject する。
//   ストックオブジェクト（GetStockObject）は所有させないこと（削除してはならない）。
class ScopedGdiObject {
public:
    ScopedGdiObject() = default;
    explicit ScopedGdiObject(HGDIOBJ obj) : obj_(obj) {}
    ~ScopedGdiObject() { Reset(); }

    ScopedGdiObject(ScopedGdiObject&& other) noexcept : obj_(other.Release()) {}
    ScopedGdiObject& operator=(ScopedGdiObject&& other) noexcept {
        if (this != &other) {
            Reset();
            obj_ = other.Release();
        }
        return *this;
    }

    ScopedGdiObject(const ScopedGdiObject&) = delete;
    ScopedGdiObject& operator=(const ScopedGdiObject&) = delete;

    bool    Valid() const { return obj_ != nullptr; }
    HGDIOBJ Get() const { return obj_; }

    // HFONT 等の具体型で取り出す（GetAs<HFONT>()）。
    template <typename T>
    T GetAs() const { return static_cast<T>(obj_); }

    HGDIOBJ Release() {
        HGDIOBJ obj = obj_;
        obj_ = nullptr;
        return obj;
    }

    void Reset(HGDIOBJ obj = nullptr) {
        if (obj == obj_) { return; }
        if (obj_ != nullptr) { ::DeleteObject(obj_); }
        obj_ = obj;
    }

private:
    HGDIOBJ obj_ = nullptr;
};

// HDC への選択を復帰する（生 HDC 版）。所有はしない＝破棄は ScopedGdiObject 側の責務。
//   領域(HRGN)には使わないこと。SelectObject は領域選択時に旧オブジェクトではなく
//   領域種別(SIMPLEREGION 等)を返すため、この復帰方式が成立しない。
class ScopedSelectHdc {
public:
    ScopedSelectHdc(HDC hdc, HGDIOBJ obj) : hdc_(hdc) {
        if (hdc_ != nullptr && obj != nullptr) {
            HGDIOBJ prev = ::SelectObject(hdc_, obj);
            // SelectObject は失敗時に nullptr、領域(HRGN)では HGDI_ERROR を返す。
            //   どちらも「戻すべき前オブジェクト」ではないため復帰対象にしない。
            if (prev != HGDI_ERROR) { old_ = prev; }
        }
    }
    ~ScopedSelectHdc() {
        if (old_ != nullptr) { ::SelectObject(hdc_, old_); }
    }

    ScopedSelectHdc(const ScopedSelectHdc&) = delete;
    ScopedSelectHdc& operator=(const ScopedSelectHdc&) = delete;

    // 選択に成功したか（フォント生成失敗時などは false）。
    bool Selected() const { return old_ != nullptr; }

private:
    HDC     hdc_;
    HGDIOBJ old_ = nullptr;
};

// CDC へのフォント選択を復帰する（MFC 版）。
//   CDC::SelectObject(CFont*) は **virtual** で、印刷プレビュー時は CPreviewDC が
//   オーバーライドしてプレビュー倍率に合わせた代替フォントを選択する。
//   CGdiObject* 版（非 virtual）を使うとこの差し替えを迂回してプレビューの文字寸法が
//   崩れるため、フォントは必ずこのクラス（CFont* オーバーロード）を使うこと。
class ScopedSelectFont {
public:
    ScopedSelectFont(CDC* dc, CFont* font) : dc_(dc) {
        if (dc_ != nullptr && font != nullptr && font->GetSafeHandle() != nullptr) {
            old_ = dc_->SelectObject(font);
        }
    }
    ~ScopedSelectFont() {
        if (old_ != nullptr) { dc_->SelectObject(old_); }
    }

    ScopedSelectFont(const ScopedSelectFont&) = delete;
    ScopedSelectFont& operator=(const ScopedSelectFont&) = delete;

    bool Selected() const { return old_ != nullptr; }

private:
    CDC*   dc_;
    CFont* old_ = nullptr;
};

// CDC へのフォント以外（ビットマップ等）の選択を復帰する（MFC 版）。
//   CDC::SelectObject は出力DCと属性DCの双方へ選択するため、生 HDC 版ではなくこちらを使う。
//   フォントには使わないこと（上記 ScopedSelectFont を使う）。
class ScopedSelectObject {
public:
    ScopedSelectObject(CDC* dc, CGdiObject* obj) : dc_(dc) {
        if (dc_ != nullptr && obj != nullptr && obj->GetSafeHandle() != nullptr) {
            old_ = dc_->SelectObject(obj);
        }
    }
    ~ScopedSelectObject() {
        if (old_ != nullptr) { dc_->SelectObject(old_); }
    }

    ScopedSelectObject(const ScopedSelectObject&) = delete;
    ScopedSelectObject& operator=(const ScopedSelectObject&) = delete;

    bool Selected() const { return old_ != nullptr; }

private:
    CDC*        dc_;
    CGdiObject* old_ = nullptr;
};

}  // namespace stirling
