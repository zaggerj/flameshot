// Capture one window by its HWND using PrintWindow (PW_RENDERFULLCONTENT).
// Unlike a screen grab this works even when the window is covered by others,
// which is the normal state of a config window on a busy remote desktop.
// Usage: wincap <hwnd> <out.png>
#include <QGuiApplication>
#include <QImage>
#include <QString>
#include <windows.h>
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    if (argc < 3) {
        printf("usage: wincap <hwnd> <out.png>\n");
        return 2;
    }
    const HWND hwnd = reinterpret_cast<HWND>(static_cast<uintptr_t>(
      strtoull(argv[1], nullptr, 10)));
    if (!hwnd || !IsWindow(hwnd)) {
        printf("invalid hwnd\n");
        return 3;
    }
    RECT r{};
    GetWindowRect(hwnd, &r);
    const int w = r.right - r.left;
    const int h = r.bottom - r.top;
    if (w <= 0 || h <= 0) {
        printf("empty window\n");
        return 4;
    }

    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp =
      CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    const BOOL printed = PrintWindow(hwnd, mem, 2 /* PW_RENDERFULLCONTENT */);
    SelectObject(mem, old);
    const QImage img(static_cast<uchar*>(bits), w, h, QImage::Format_RGB32);
    const bool saved =
      printed && img.copy().save(QString::fromLocal8Bit(argv[2]), "PNG");
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    printf("hwnd=%p print=%d saved=%d %dx%d\n",
           reinterpret_cast<void*>(hwnd),
           static_cast<int>(printed),
           static_cast<int>(saved),
           w,
           h);
    return saved ? 0 : 5;
}
