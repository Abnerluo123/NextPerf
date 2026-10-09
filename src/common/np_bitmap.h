// CPU 侧面板位图：用 Direct2D 把面板画进一张 BGRA DIB。
//
// 同一个类被三处复用：
//   * 桌面分层叠加窗口（UpdateLayeredWindow）
//   * D3D11 游戏内叠加（UpdateSubresource）
//   * D3D12 游戏内叠加（上传堆 + CopyTextureRegion）
// 这样三种显示路径的内容与排版完全一致。
#pragma once

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <cstdint>

namespace npb {

bool GfxInit();
void GfxShutdown();
ID2D1Factory* Factory();
IDWriteFactory* DWrite();

// 位图光栅化倍率：1.0 = 96DPI。
// 设成系统 DPI/96 之后，面板会按物理像素渲染，高分屏上不会显得过小，
// 而且 D2D 是矢量绘制，放大后依然锐利。
void  SetRasterScale(float scale);
float RasterScale();

class PanelBitmap {
public:
    // w/h 传的是逻辑尺寸（DIP）；内部按光栅倍率换算成像素
    bool  Ensure(int w, int h);
    void  Release();
    // 回调里负责往渲染目标上画东西
    bool  Render(int w, int h, void (*draw)(ID2D1RenderTarget*, void*), void* ud);

    const unsigned char* pixels() const { return pixels_; }
    int   width() const { return w_; }     // 像素宽
    int   height() const { return h_; }    // 像素高
    int   stride() const { return stride_; }
    HDC   dc() const { return hdc_; }   // 供 UpdateLayeredWindow 直接使用

private:
    HDC          hdc_ = nullptr;
    HBITMAP      dib_ = nullptr;
    HBITMAP      oldBmp_ = nullptr;
    void*        bits_ = nullptr;
    unsigned char* pixels_ = nullptr;
    int          w_ = 0, h_ = 0, stride_ = 0;
    ID2D1DCRenderTarget* rt_ = nullptr;
};

}  // namespace npb
