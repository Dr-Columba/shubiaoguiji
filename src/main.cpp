#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <shlobj.h>
#include <gdiplus.h>

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

using Gdiplus::Color;
using Gdiplus::Graphics;
using Gdiplus::Pen;
using Gdiplus::SolidBrush;
using Gdiplus::SmoothingModeAntiAlias;
using Gdiplus::LineCapRound;
using Gdiplus::LineCapTriangle;

namespace {

constexpr wchar_t kWindowClassName[] = L"MouseTrailOverlayWindow";
constexpr wchar_t kTrayTip[] = L"鼠标轨迹";
constexpr UINT WMAPP_TRAY = WM_APP + 1;
constexpr UINT_PTR kRenderTimerId = 1;
constexpr UINT kFrameMsActive = 16; // 60 fps
constexpr UINT kFrameMsRainbowIdle = 33; // ~30 fps
constexpr UINT kFrameMsSleep = 50; // 20 fps polling
constexpr int kTrailSlots = 300;
constexpr int kRainbowStep = 25;
constexpr float kRainbowStepIntervalMs = 16.6667f;

enum MenuId : UINT {
    IDM_COLOR_CUSTOM = 1001,
    IDM_COLOR_RAINBOW,
    IDM_THICKNESS_THICK,
    IDM_THICKNESS_NORMAL,
    IDM_THICKNESS_THIN,
    IDM_TRAIL_SOFT,
    IDM_TRAIL_NORMAL,
    IDM_TRAIL_FAST,
    IDM_OPACITY_HIGH,
    IDM_OPACITY_MED,
    IDM_OPACITY_LOW,
    IDM_ONLY_DRAG,
    IDM_EXIT,
};

struct Settings {
    bool useCustomColor = false;      // colorsentaku.Checked
    COLORREF customColor = RGB(0, 0, 0);

    int thickness = 20;               // hutosa
    float fadeStep = 1.0f;            // herasi
    BYTE globalOpacity = 230;         // 0.9
    bool onlyDrag = false;

    bool thickChecked() const { return thickness == 50; }
    bool normalChecked() const { return thickness == 20; }
    bool thinChecked() const { return thickness == 7; }

    bool trailSoftChecked() const { return std::fabs(fadeStep - 0.5f) < 0.001f; }
    bool trailNormalChecked() const { return std::fabs(fadeStep - 1.0f) < 0.001f; }
    bool trailFastChecked() const { return std::fabs(fadeStep - 2.0f) < 0.001f; }

    bool opacityHighChecked() const { return globalOpacity == 230; }
    bool opacityMediumChecked() const { return globalOpacity == 128; }
    bool opacityLowChecked() const { return globalOpacity == 38; }
};

struct TrailPoint {
    POINT pt{0, 0};
    float size = 20.0f;
    bool visible = false;
    COLORREF color = RGB(255, 0, 0);
};

struct DibBuffer {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ oldObj = nullptr;
    void* pixels = nullptr;
    int width = 0;
    int height = 0;

    ~DibBuffer() { destroy(); }

    void destroy() {
        if (dc && oldObj) {
            SelectObject(dc, oldObj);
            oldObj = nullptr;
        }
        if (bmp) {
            DeleteObject(bmp);
            bmp = nullptr;
        }
        if (dc) {
            DeleteDC(dc);
            dc = nullptr;
        }
        pixels = nullptr;
        width = 0;
        height = 0;
    }

    bool ensure(int w, int h) {
        if (w <= 0 || h <= 0) return false;
        if (w == width && h == height && dc && bmp && pixels) return true;

        destroy();

        dc = CreateCompatibleDC(nullptr);
        if (!dc) return false;

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (!bmp || !pixels) {
            destroy();
            return false;
        }

        oldObj = SelectObject(dc, bmp);
        width = w;
        height = h;
        return true;
    }

    void clearTransparent() {
        if (!pixels || width <= 0 || height <= 0) return;
        std::memset(pixels, 0, static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    }
};

class OverlayApp {
public:
    explicit OverlayApp(HINSTANCE inst) : instance_(inst) {}
    ~OverlayApp() { shutdown(); }

    int run() {
        if (!init()) return 1;

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
    }

private:
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;

    NOTIFYICONDATAW nid_{};
    HMENU rootMenu_ = nullptr;
    HMENU colorMenu_ = nullptr;
    HMENU thicknessMenu_ = nullptr;
    HMENU trailMenu_ = nullptr;
    HMENU opacityMenu_ = nullptr;

    ULONG_PTR gdiplusToken_ = 0;

    Settings settings_{};
    DibBuffer dib_{};

    std::array<TrailPoint, kTrailSlots> points_{};
    int writeIndex_ = 0;

    POINT cursorPos_{0, 0};
    bool haveCursor_ = false;
    bool leftDown_ = false;
    bool colorDialogOpen_ = false;
    bool rendererRunning_ = false;
    UINT currentTimerMs_ = 0;

    int rainbowState_ = 0;
    int rbR_ = 255;
    int rbG_ = 0;
    int rbB_ = 0;
    float rainbowStepAccumMs_ = 0.0f;

    RECT lastWindowRect_{0, 0, 0, 0};
    ULONGLONG lastInputTick_ = 0;
    std::chrono::steady_clock::time_point lastFrameTime_ = std::chrono::steady_clock::now();

    static OverlayApp*& self() {
        static OverlayApp* s = nullptr;
        return s;
    }

    bool init() {
        self() = this;

        Gdiplus::GdiplusStartupInput gdiplusStartupInput;
        if (Gdiplus::GdiplusStartup(&gdiplusToken_, &gdiplusStartupInput, nullptr) != Gdiplus::Ok) {
            return false;
        }

        loadSettings();
        resetTrail();

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &OverlayApp::wndProcStatic;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        wc.lpszClassName = kWindowClassName;
        if (!RegisterClassExW(&wc)) {
            return false;
        }

        const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
        const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);

        hwnd_ = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_LAYERED,
            kWindowClassName,
            L"",
            WS_POPUP,
            vx,
            vy,
            vw,
            vh,
            nullptr,
            nullptr,
            instance_,
            nullptr
        );

        if (!hwnd_) return false;

        ShowWindow(hwnd_, SW_HIDE);

        if (!createTrayIcon()) return false;
        createMenu();
        refreshMenuChecks();

        POINT p{};
        if (GetCursorPos(&p)) {
            cursorPos_ = p;
            haveCursor_ = true;
        }
        lastInputTick_ = GetTickCount64();

        // Keep behavior identical to original: default is rainbow mode and always visible dot.
        wakeRenderer();
        return true;
    }

    void shutdown() {
        stopRenderer();

        if (nid_.cbSize != 0) {
            Shell_NotifyIconW(NIM_DELETE, &nid_);
            nid_ = {};
        }

        if (rootMenu_) {
            DestroyMenu(rootMenu_);
            rootMenu_ = nullptr;
            colorMenu_ = nullptr;
            thicknessMenu_ = nullptr;
            trailMenu_ = nullptr;
            opacityMenu_ = nullptr;
        }

        dib_.destroy();

        if (hwnd_) {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }

        if (gdiplusToken_) {
            Gdiplus::GdiplusShutdown(gdiplusToken_);
            gdiplusToken_ = 0;
        }

        self() = nullptr;
    }

    bool createTrayIcon() {
        nid_ = {};
        nid_.cbSize = sizeof(nid_);
        nid_.hWnd = hwnd_;
        nid_.uID = 1;
        nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid_.uCallbackMessage = WMAPP_TRAY;
        nid_.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        lstrcpynW(nid_.szTip, kTrayTip, ARRAYSIZE(nid_.szTip));
        return Shell_NotifyIconW(NIM_ADD, &nid_) == TRUE;
    }

    void createMenu() {
        rootMenu_ = CreatePopupMenu();
        colorMenu_ = CreatePopupMenu();
        thicknessMenu_ = CreatePopupMenu();
        trailMenu_ = CreatePopupMenu();
        opacityMenu_ = CreatePopupMenu();

        AppendMenuW(colorMenu_, MF_STRING, IDM_COLOR_CUSTOM, L"自定义颜色");
        AppendMenuW(colorMenu_, MF_STRING, IDM_COLOR_RAINBOW, L"彩虹色");

        AppendMenuW(thicknessMenu_, MF_STRING, IDM_THICKNESS_THICK, L"粗");
        AppendMenuW(thicknessMenu_, MF_STRING, IDM_THICKNESS_NORMAL, L"中");
        AppendMenuW(thicknessMenu_, MF_STRING, IDM_THICKNESS_THIN, L"细");

        AppendMenuW(trailMenu_, MF_STRING, IDM_TRAIL_SOFT, L"明显");
        AppendMenuW(trailMenu_, MF_STRING, IDM_TRAIL_NORMAL, L"适中");
        AppendMenuW(trailMenu_, MF_STRING, IDM_TRAIL_FAST, L"轻微");

        AppendMenuW(opacityMenu_, MF_STRING, IDM_OPACITY_HIGH, L"高");
        AppendMenuW(opacityMenu_, MF_STRING, IDM_OPACITY_MED, L"中");
        AppendMenuW(opacityMenu_, MF_STRING, IDM_OPACITY_LOW, L"低");

        AppendMenuW(rootMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(colorMenu_), L"颜色");
        AppendMenuW(rootMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(thicknessMenu_), L"粗细");
        AppendMenuW(rootMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(trailMenu_), L"拖尾残留");
        AppendMenuW(rootMenu_, MF_POPUP, reinterpret_cast<UINT_PTR>(opacityMenu_), L"透明度");
        AppendMenuW(rootMenu_, MF_STRING, IDM_ONLY_DRAG, L"仅拖拽时显示");
        AppendMenuW(rootMenu_, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(rootMenu_, MF_STRING, IDM_EXIT, L"退出");
    }

    void refreshMenuChecks() {
        if (!rootMenu_) return;

        CheckMenuRadioItem(colorMenu_, IDM_COLOR_CUSTOM, IDM_COLOR_RAINBOW,
                           settings_.useCustomColor ? IDM_COLOR_CUSTOM : IDM_COLOR_RAINBOW, MF_BYCOMMAND);

        UINT thickId = IDM_THICKNESS_NORMAL;
        if (settings_.thickChecked()) thickId = IDM_THICKNESS_THICK;
        else if (settings_.thinChecked()) thickId = IDM_THICKNESS_THIN;
        CheckMenuRadioItem(thicknessMenu_, IDM_THICKNESS_THICK, IDM_THICKNESS_THIN, thickId, MF_BYCOMMAND);

        UINT trailId = IDM_TRAIL_NORMAL;
        if (settings_.trailSoftChecked()) trailId = IDM_TRAIL_SOFT;
        else if (settings_.trailFastChecked()) trailId = IDM_TRAIL_FAST;
        CheckMenuRadioItem(trailMenu_, IDM_TRAIL_SOFT, IDM_TRAIL_FAST, trailId, MF_BYCOMMAND);

        UINT opacityId = IDM_OPACITY_HIGH;
        if (settings_.opacityMediumChecked()) opacityId = IDM_OPACITY_MED;
        else if (settings_.opacityLowChecked()) opacityId = IDM_OPACITY_LOW;
        CheckMenuRadioItem(opacityMenu_, IDM_OPACITY_HIGH, IDM_OPACITY_LOW, opacityId, MF_BYCOMMAND);

        CheckMenuItem(rootMenu_, IDM_ONLY_DRAG,
                      MF_BYCOMMAND | (settings_.onlyDrag ? MF_CHECKED : MF_UNCHECKED));
    }

    static LRESULT CALLBACK wndProcStatic(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        OverlayApp* app = self();
        if (!app) return DefWindowProcW(hwnd, msg, wParam, lParam);
        return app->wndProc(hwnd, msg, wParam, lParam);
    }

    LRESULT wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT;

        case WM_TIMER:
            if (wParam == kRenderTimerId) {
                renderFrame();
                return 0;
            }
            break;

        case WM_COMMAND:
            handleMenuCommand(LOWORD(wParam));
            return 0;

        case WMAPP_TRAY:
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                showTrayMenu();
            }
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        default:
            break;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    void setRenderTimer(UINT ms) {
        if (!hwnd_ || ms == 0) return;
        if (rendererRunning_ && currentTimerMs_ == ms) return;
        if (rendererRunning_) {
            KillTimer(hwnd_, kRenderTimerId);
        }
        SetTimer(hwnd_, kRenderTimerId, ms, nullptr);
        rendererRunning_ = true;
        currentTimerMs_ = ms;
    }

    void wakeRenderer() {
        setRenderTimer(kFrameMsActive);
        lastFrameTime_ = std::chrono::steady_clock::now();
    }

    void stopRenderer() {
        if (rendererRunning_ && hwnd_) {
            KillTimer(hwnd_, kRenderTimerId);
            rendererRunning_ = false;
            currentTimerMs_ = 0;
        }
    }

    void handleMenuCommand(UINT id) {
        switch (id) {
        case IDM_COLOR_CUSTOM:
            pickCustomColor();
            break;
        case IDM_COLOR_RAINBOW:
            settings_.useCustomColor = false;
            saveSettings();
            break;

        case IDM_THICKNESS_THICK:
            settings_.thickness = 50;
            resetTrail();
            saveSettings();
            break;
        case IDM_THICKNESS_NORMAL:
            settings_.thickness = 20;
            resetTrail();
            saveSettings();
            break;
        case IDM_THICKNESS_THIN:
            settings_.thickness = 7;
            resetTrail();
            saveSettings();
            break;

        case IDM_TRAIL_SOFT:
            settings_.fadeStep = 0.5f;
            saveSettings();
            break;
        case IDM_TRAIL_NORMAL:
            settings_.fadeStep = 1.0f;
            saveSettings();
            break;
        case IDM_TRAIL_FAST:
            settings_.fadeStep = 2.0f;
            saveSettings();
            break;

        case IDM_OPACITY_HIGH:
            settings_.globalOpacity = 230;
            saveSettings();
            break;
        case IDM_OPACITY_MED:
            settings_.globalOpacity = 128;
            saveSettings();
            break;
        case IDM_OPACITY_LOW:
            settings_.globalOpacity = 38;
            saveSettings();
            break;

        case IDM_ONLY_DRAG:
            settings_.onlyDrag = !settings_.onlyDrag;
            saveSettings();
            break;

        case IDM_EXIT:
            DestroyWindow(hwnd_);
            break;

        default:
            break;
        }

        refreshMenuChecks();
        wakeRenderer();
    }

    void pickCustomColor() {
        colorDialogOpen_ = true;
        CHOOSECOLORW cc{};
        COLORREF custom[16]{};
        cc.lStructSize = sizeof(cc);
        cc.hwndOwner = hwnd_;
        cc.rgbResult = settings_.customColor;
        cc.lpCustColors = custom;
        cc.Flags = CC_RGBINIT | CC_FULLOPEN;
        if (ChooseColorW(&cc) == TRUE) {
            settings_.customColor = cc.rgbResult;
            settings_.useCustomColor = true;
            saveSettings();
        }
        colorDialogOpen_ = false;
    }

    void showTrayMenu() {
        refreshMenuChecks();

        POINT pt{};
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd_);
        TrackPopupMenu(rootMenu_, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
        PostMessageW(hwnd_, WM_NULL, 0, 0);
    }

    void resetTrail() {
        for (auto& p : points_) {
            p.visible = false;
            p.size = static_cast<float>(settings_.thickness);
            p.pt = {0, 0};
            p.color = RGB(255, 0, 0);
        }
        writeIndex_ = 0;
    }

    void updateRainbow() {
        if (rainbowState_ == 0) {
            rbG_ += kRainbowStep;
            if (rbG_ >= 255) { rainbowState_ = 1; rbG_ = 255; }
        } else if (rainbowState_ == 1) {
            rbR_ -= kRainbowStep;
            if (rbR_ <= 0) { rainbowState_ = 2; rbR_ = 0; }
        } else if (rainbowState_ == 2) {
            rbB_ += kRainbowStep;
            if (rbB_ >= 255) { rainbowState_ = 3; rbB_ = 255; }
        } else if (rainbowState_ == 3) {
            rbG_ -= kRainbowStep;
            if (rbG_ <= 0) { rainbowState_ = 4; rbG_ = 0; }
        } else if (rainbowState_ == 4) {
            rbR_ += kRainbowStep;
            if (rbR_ >= 255) { rainbowState_ = 5; rbR_ = 255; }
        } else {
            rbB_ -= kRainbowStep;
            if (rbB_ <= 0) { rainbowState_ = 0; rbB_ = 0; }
        }
    }

    RECT computeDrawBounds(bool includeCursorDot) const {
        bool hasAny = false;
        int minX = 0, minY = 0, maxX = 0, maxY = 0;

        auto consume = [&](int x, int y, int r) {
            if (!hasAny) {
                minX = x - r;
                minY = y - r;
                maxX = x + r;
                maxY = y + r;
                hasAny = true;
            } else {
                minX = std::min(minX, x - r);
                minY = std::min(minY, y - r);
                maxX = std::max(maxX, x + r);
                maxY = std::max(maxY, y + r);
            }
        };

        for (const auto& p : points_) {
            if (!p.visible) continue;
            int rad = static_cast<int>(std::ceil(std::max(1.0f, p.size))) + 6;
            consume(p.pt.x, p.pt.y, rad);
        }

        if (includeCursorDot && haveCursor_) {
            int rad = (settings_.thickness / 2) + 8;
            consume(cursorPos_.x, cursorPos_.y, rad);
        }

        if (!hasAny) return RECT{0, 0, 0, 0};

        const LONG vx = static_cast<LONG>(GetSystemMetrics(SM_XVIRTUALSCREEN));
        const LONG vy = static_cast<LONG>(GetSystemMetrics(SM_YVIRTUALSCREEN));
        const LONG vw = static_cast<LONG>(GetSystemMetrics(SM_CXVIRTUALSCREEN));
        const LONG vh = static_cast<LONG>(GetSystemMetrics(SM_CYVIRTUALSCREEN));
        const LONG rx2 = vx + vw;
        const LONG ry2 = vy + vh;

        RECT rc{minX, minY, maxX, maxY};
        rc.left = std::max(rc.left, vx);
        rc.top = std::max(rc.top, vy);
        rc.right = std::min(rc.right, rx2);
        rc.bottom = std::min(rc.bottom, ry2);
        if (rc.right <= rc.left || rc.bottom <= rc.top) {
            return RECT{0, 0, 0, 0};
        }
        return rc;
    }

    bool hasVisibleTrail() const {
        for (const auto& p : points_) {
            if (p.visible) return true;
        }
        return false;
    }

    void renderFrame() {
        POINT p{};
        bool moved = false;
        bool leftDownPrev = leftDown_;
        if (GetCursorPos(&p)) {
            moved = (!haveCursor_ || p.x != cursorPos_.x || p.y != cursorPos_.y);
            cursorPos_ = p;
            haveCursor_ = true;
            if (moved) {
                lastInputTick_ = GetTickCount64();
            }
        } else if (!haveCursor_) {
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        const float dtMs = std::chrono::duration<float, std::milli>(now - lastFrameTime_).count();
        lastFrameTime_ = now;
        const float frameScale = std::clamp(dtMs / 16.6667f, 0.25f, 3.0f);

        leftDown_ = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        const bool leftDownChanged = (leftDown_ != leftDownPrev);

        const bool drawCursorDot = !settings_.onlyDrag;
        const bool shouldAnimateRainbow = !settings_.useCustomColor;
        const bool shouldDrawTrailNow = !settings_.onlyDrag || leftDown_;

        if (!colorDialogOpen_) {
            if (moved && shouldDrawTrailNow) {
                TrailPoint& slot = points_[writeIndex_];
                slot.visible = true;
                slot.pt = cursorPos_;
                slot.size = static_cast<float>(settings_.thickness);
                slot.color = RGB(rbR_, rbG_, rbB_);
                writeIndex_ = (writeIndex_ + 1) % kTrailSlots;
            }

            for (auto& p : points_) {
                if (p.size <= 0.0f) {
                    p.visible = false;
                    p.size = static_cast<float>(settings_.thickness);
                }
                if (p.visible) {
                    p.size -= settings_.fadeStep * frameScale;
                }
            }
        }

        const bool activeTrail = hasVisibleTrail();
        const bool rainbowVisible = drawCursorDot || activeTrail || (settings_.onlyDrag && leftDown_);
        int rainbowSteps = 0;
        if (shouldAnimateRainbow && rainbowVisible) {
            rainbowStepAccumMs_ += dtMs;
            while (rainbowStepAccumMs_ >= kRainbowStepIntervalMs) {
                updateRainbow();
                rainbowStepAccumMs_ -= kRainbowStepIntervalMs;
                ++rainbowSteps;
                if (rainbowSteps >= 8) break;
            }
        } else {
            rainbowStepAccumMs_ = 0.0f;
        }

        const bool needsVisualUpdate = moved || leftDownChanged || activeTrail || (rainbowSteps > 0);
        if (!needsVisualUpdate) {
            if (!drawCursorDot && !activeTrail) {
                ShowWindow(hwnd_, SW_HIDE);
            }
            UINT desired = kFrameMsSleep;
            if (shouldAnimateRainbow && drawCursorDot) {
                desired = kFrameMsRainbowIdle;
            }
            setRenderTimer(desired);
            return;
        }

        RECT rc = computeDrawBounds(drawCursorDot);
        if (rc.right <= rc.left || rc.bottom <= rc.top) {
            ShowWindow(hwnd_, SW_HIDE);
            setRenderTimer(kFrameMsSleep);
            return;
        }

        const int width = rc.right - rc.left;
        const int height = rc.bottom - rc.top;
        if (!dib_.ensure(width, height)) {
            return;
        }

        dib_.clearTransparent();

        {
            Graphics g(dib_.dc);
            g.SetSmoothingMode(SmoothingModeAntiAlias);

            for (int i = 0; i < kTrailSlots; ++i) {
                const TrailPoint& p = points_[i];
                if (!p.visible) continue;

                const int prev = (i == 0) ? (kTrailSlots - 1) : (i - 1);
                const TrailPoint& q = points_[prev];

                COLORREF c = settings_.useCustomColor ? settings_.customColor : p.color;
                Color lineColor(255, GetRValue(c), GetGValue(c), GetBValue(c));

                float penW = std::max(1.0f, p.size);
                Pen pen(lineColor, penW);
                pen.SetStartCap(LineCapRound);
                pen.SetEndCap(LineCapTriangle);

                const float x1 = static_cast<float>(p.pt.x - rc.left);
                const float y1 = static_cast<float>(p.pt.y - rc.top);
                const float x2 = static_cast<float>(q.pt.x - rc.left);
                const float y2 = static_cast<float>(q.pt.y - rc.top);
                g.DrawLine(&pen, x1, y1, x2, y2);
            }

            if (drawCursorDot && haveCursor_) {
                COLORREF c = settings_.useCustomColor ? settings_.customColor : RGB(rbR_, rbG_, rbB_);
                Color dotColor(255, GetRValue(c), GetGValue(c), GetBValue(c));
                SolidBrush brush(dotColor);
                const int r = settings_.thickness / 2;
                const int cx = cursorPos_.x - rc.left;
                const int cy = cursorPos_.y - rc.top;
                g.FillEllipse(&brush,
                              static_cast<Gdiplus::REAL>(cx - r),
                              static_cast<Gdiplus::REAL>(cy - r),
                              static_cast<Gdiplus::REAL>(settings_.thickness),
                              static_cast<Gdiplus::REAL>(settings_.thickness));
            }
        }

        SIZE sz{width, height};
        POINT src{0, 0};
        POINT dst{rc.left, rc.top};

        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = settings_.globalOpacity;
        blend.AlphaFormat = AC_SRC_ALPHA;

        UpdateLayeredWindow(hwnd_, nullptr, &dst, &sz, dib_.dc, &src, 0, &blend, ULW_ALPHA);

        const UINT swpFlags = SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOSENDCHANGING;
        SetWindowPos(hwnd_, HWND_TOPMOST, rc.left, rc.top, width, height, swpFlags);
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        lastWindowRect_ = rc;

        UINT desired = kFrameMsSleep;
        if (moved || leftDown_ || activeTrail) {
            desired = kFrameMsActive;
        } else if (shouldAnimateRainbow && drawCursorDot) {
            desired = kFrameMsRainbowIdle;
        }
        setRenderTimer(desired);
    }

    static std::wstring trim(const std::wstring& s) {
        size_t b = 0;
        while (b < s.size() && iswspace(s[b])) ++b;
        size_t e = s.size();
        while (e > b && iswspace(s[e - 1])) --e;
        return s.substr(b, e - b);
    }

    static bool parseBool(const std::wstring& s) {
        std::wstring t;
        t.reserve(s.size());
        for (wchar_t ch : s) t.push_back(static_cast<wchar_t>(towlower(ch)));
        return t == L"true" || t == L"1";
    }

    std::filesystem::path configBaseDir() const {
        PWSTR docs = nullptr;
        std::filesystem::path base;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
            base = docs;
            CoTaskMemFree(docs);
        } else {
            wchar_t path[MAX_PATH]{};
            SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, path);
            base = path;
        }
        return base;
    }

    std::filesystem::path configDir() const {
        return configBaseDir() / L"鼠标轨迹";
    }

    std::filesystem::path legacyConfigDir() const {
        return configBaseDir() / L"残像マウス";
    }

    std::filesystem::path configPath() const {
        return configDir() / L"config.txt";
    }

    std::filesystem::path legacyConfigPath() const {
        return legacyConfigDir() / L"config.txt";
    }

    void loadSettings() {
        auto path = configPath();
        if (!std::filesystem::exists(path)) {
            path = legacyConfigPath();
        }

        std::ifstream in(path);
        if (!in.good()) {
            return;
        }

        std::string line;
        std::getline(in, line);
        in.close();
        if (line.empty()) return;

        std::vector<std::string> tokens;
        std::stringstream ss(line);
        std::string item;
        while (std::getline(ss, item, '|')) {
            tokens.push_back(item);
        }
        if (tokens.size() < 12) return;

        auto toBool = [](const std::string& s) -> bool {
            std::string t;
            t.reserve(s.size());
            for (char c : s) t.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            return t == "true" || t == "1";
        };

        settings_.useCustomColor = toBool(tokens[0]);
        try {
            int ole = std::stoi(tokens[1]);
            settings_.customColor = static_cast<COLORREF>(ole);
        } catch (...) {
        }

        if (toBool(tokens[2])) settings_.thickness = 50;
        if (toBool(tokens[3])) settings_.thickness = 20;
        if (toBool(tokens[4])) settings_.thickness = 7;

        if (toBool(tokens[5])) settings_.fadeStep = 0.5f;
        if (toBool(tokens[6])) settings_.fadeStep = 1.0f;
        if (toBool(tokens[7])) settings_.fadeStep = 2.0f;

        if (toBool(tokens[8])) settings_.globalOpacity = 230;
        if (toBool(tokens[9])) settings_.globalOpacity = 128;
        if (toBool(tokens[10])) settings_.globalOpacity = 38;

        settings_.onlyDrag = toBool(tokens[11]);
    }

    void saveSettings() {
        auto dir = configDir();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        std::ofstream out(configPath(), std::ios::trunc);
        if (!out.good()) return;

        auto b = [](bool v) { return v ? "True" : "False"; };
        const bool thick = settings_.thickChecked();
        const bool normal = settings_.normalChecked();
        const bool thin = settings_.thinChecked();
        const bool tSoft = settings_.trailSoftChecked();
        const bool tNormal = settings_.trailNormalChecked();
        const bool tFast = settings_.trailFastChecked();
        const bool oHigh = settings_.opacityHighChecked();
        const bool oMed = settings_.opacityMediumChecked();
        const bool oLow = settings_.opacityLowChecked();

        out << b(settings_.useCustomColor) << "|"
            << static_cast<int>(settings_.customColor) << "|"
            << b(thick) << "|"
            << b(normal) << "|"
            << b(thin) << "|"
            << b(tSoft) << "|"
            << b(tNormal) << "|"
            << b(tFast) << "|"
            << b(oHigh) << "|"
            << b(oMed) << "|"
            << b(oLow) << "|"
            << b(settings_.onlyDrag);

        out.close();
    }
};

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
    OverlayApp app(hInstance);
    return app.run();
}
