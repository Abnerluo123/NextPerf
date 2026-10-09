#include "np_bitmap.h"

#include <dwrite.h>
#include <cstring>

namespace npb {

static ID2D1Factory* gD2D = nullptr;
static IDWriteFactory* gDW = nullptr;
static float gRasterScale = 1.0f;

void SetRasterScale(float scale) {
    if (scale > 0.05f && scale < 8.0f) gRasterScale = scale;
}
float RasterScale() { return gRasterScale; }

bool GfxInit() {
    if (gD2D) return true;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &gD2D))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&gDW)))) {
        gD2D->Release();
        gD2D = nullptr;
        return false;
    }
    return true;
}

void GfxShutdown() {
    if (gD2D) { gD2D->Release(); gD2D = nullptr; }
    if (gDW) { gDW->Release(); gDW = nullptr; }
}

ID2D1Factory* Factory() { return gD2D; }
IDWriteFactory* DWrite() { return gDW; }

void PanelBitmap::Release() {
    if (rt_) { rt_->Release(); rt_ = nullptr; }
    if (hdc_) {
        if (oldBmp_) SelectObject(hdc_, oldBmp_);
        DeleteDC(hdc_);
        hdc_ = nullptr;
    }
    if (dib_) { DeleteObject(dib_); dib_ = nullptr; }
    bits_ = nullptr;
    pixels_ = nullptr;
    w_ = h_ = stride_ = 0;
    oldBmp_ = nullptr;
}

bool PanelBitmap::Ensure(int w, int h) {
    // 入参是逻辑尺寸（DIP），这里换算成物理像素
    int pw = (int)(w * gRasterScale + 0.5f);
    int ph = (int)(h * gRasterScale + 0.5f);
    if (w_ == pw && h_ == ph && rt_) return true;
    Release();
    if (pw <= 0 || ph <= 0) return false;
    if (!gD2D) return false;

    HDC screen = GetDC(nullptr);
    hdc_ = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!hdc_) return false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = pw;
    bmi.bmiHeader.biHeight = -ph;   // 自上而下，方便直接上传
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    dib_ = CreateDIBSection(hdc_, &bmi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    if (!dib_ || !bits_) { Release(); return false; }
    oldBmp_ = reinterpret_cast<HBITMAP>(SelectObject(hdc_, dib_));
    pixels_ = reinterpret_cast<unsigned char*>(bits_);
    w_ = pw;
    h_ = ph;
    stride_ = pw * 4;

    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f, D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
    if (FAILED(gD2D->CreateDCRenderTarget(&props, &rt_))) { Release(); return false; }
    return true;
}

bool PanelBitmap::Render(int w, int h, void (*draw)(ID2D1RenderTarget*, void*), void* ud) {
    if (!Ensure(w, h)) return false;
    RECT rc{0, 0, w_, h_};          // 物理像素
    if (FAILED(rt_->BindDC(hdc_, &rc))) return false;
    rt_->BeginDraw();
    // 逻辑坐标 → 物理像素：调用方按 DIP 画，内部整体放大，高分屏依然锐利
    rt_->SetTransform(D2D1::Matrix3x2F::Scale(gRasterScale, gRasterScale));
    rt_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    if (draw) draw(rt_, ud);
    rt_->SetTransform(D2D1::IdentityMatrix());
    HRESULT hr = rt_->EndDraw();
    return SUCCEEDED(hr);
}

}  // namespace npb
