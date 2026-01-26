// NTWobble.cpp : GDI rotating NT logo with color transitions

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cmath>
#include <random>
#include <array>
#include <algorithm>
#include <numbers>
#include <DirectXMath.h>
#include <cstdio>

using namespace DirectX;

constexpr float PERSPECTIVE_DIST = 400.0f;
constexpr float BASE_WIDTH = 293.0f;
constexpr float TWO_PI = std::numbers::pi_v<float> * 2.0f;
constexpr float INV_BASE_WIDTH = 0.50f / BASE_WIDTH;

// GDI resources
HDC g_hdcBack = nullptr;
HBITMAP g_hbmBack = nullptr;
HBITMAP g_hbmOld = nullptr;
HBRUSH g_hBrush = nullptr;
HBRUSH g_hBgBrush = nullptr;
int g_width = 0, g_height = 0;

// Animation state
float g_time = 0.0f;
float g_currentR, g_currentG, g_currentB;
float g_targetR, g_targetG, g_targetB;
float g_colorProgress = 0.0f;
LARGE_INTEGER g_perfFreq, g_lastTime;
bool g_isRunning = true;
bool g_debugMode = false;      // Toggle with Q key
bool g_isFullScreen = false;   // Full screen state
WINDOWPLACEMENT g_wpPrev = { sizeof(g_wpPrev) };  // Saved window placement

// Modern C++ random number generator
std::mt19937 g_rng{std::random_device{}()};
std::uniform_real_distribution<float> g_colorDist{0.0f, 1.0f};

// 2D point for polygons
struct Point2F { float x, y; };

// N polygon vertices (std::array for type safety)
constexpr std::array<Point2F, 7> g_nPoly = {{
    {-146, -93}, {-110, -93}, {-25, 32}, {-25, 93}, {-110, -32}, {-110, 93}, {-146, 93},
}};

// T polygon vertices
constexpr std::array<Point2F, 11> g_tPoly = {{
    {-26, -93}, {147, -93}, {147, -57}, {97, -57}, {97, 92}, {60, 92}, {60, -57}, {9, -57}, {9, 92}, {-27, 92}, {-26, 31},
}};

inline void RandomColorF(float& r, float& g, float& b) noexcept {
    r = g_colorDist(g_rng);
    g = g_colorDist(g_rng);
    b = g_colorDist(g_rng);
}

void ToggleFullScreen(HWND hWnd) {
    DWORD dwStyle = GetWindowLong(hWnd, GWL_STYLE);
    
    if (!g_isFullScreen) {
        MONITORINFO mi = { sizeof(mi) };
        if (GetWindowPlacement(hWnd, &g_wpPrev) &&
            GetMonitorInfo(MonitorFromWindow(hWnd, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLong(hWnd, GWL_STYLE, dwStyle & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(hWnd, HWND_TOP,
                mi.rcMonitor.left, mi.rcMonitor.top,
                mi.rcMonitor.right - mi.rcMonitor.left,
                mi.rcMonitor.bottom - mi.rcMonitor.top,
                SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
        g_isFullScreen = true;
    } else {
        SetWindowLong(hWnd, GWL_STYLE, dwStyle | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(hWnd, &g_wpPrev);
        SetWindowPos(hWnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_isFullScreen = false;
    }
}

// Transform point with 3D rotation and perspective projection
inline POINT TransformPoint(float px, float py, float cx, float cy, float cz,
    float sx, float sy, float sz, float scale, float centerX, float centerY) noexcept {
    const float y = py * cx;
    const float z1 = py * sx;
    const float x = px * cy + z1 * sy;
    const float z = z1 * cy - px * sy;
    const float nx = x * cz - y * sz;
    const float ny = x * sz + y * cz;
    const float invZ = 1.0f / (PERSPECTIVE_DIST + z);
    const float s = PERSPECTIVE_DIST * invZ * scale;
    return { static_cast<LONG>(nx * s + centerX), static_cast<LONG>(ny * s + centerY) };
}

// Simple transform for debug mode (no rotation)
inline POINT TransformPointStatic(float px, float py, float scale, float centerX, float centerY) noexcept {
    return { static_cast<LONG>(px * scale + centerX), static_cast<LONG>(py * scale + centerY) };
}

// Draw filled polygon using GDI - all stack allocated
template<size_t N> 
static void DrawFilledPolygon(
    const std::array<Point2F, N>& poly,
    float cx, float cy, float cz,
    float sx, float sy, float sz,
    float scale, float centerX, float centerY) noexcept
{
    POINT pts[N];
    
    for (size_t i = 0; i < N; ++i) {
        pts[i] = TransformPoint(poly[i].x, poly[i].y, cx, cy, cz, sx, sy, sz, scale, centerX, centerY);
    }

    ::Polygon(g_hdcBack, pts, static_cast<int>(N));
}

// Draw polygon in debug mode with XOR colors
template<size_t N>
static void DrawFilledPolygonDebugXOR(
    const std::array<Point2F, N>& poly,
    float scale, float centerX, float centerY,
    COLORREF color, POINT* outPts) noexcept
{
    for (size_t i = 0; i < N; ++i) {
        outPts[i] = TransformPointStatic(poly[i].x, poly[i].y, scale, centerX, centerY);
    }

    // Use XOR mode for drawing
    int oldRop = SetROP2(g_hdcBack, R2_XORPEN);
    
    HBRUSH hBrush = CreateSolidBrush(color);
    HPEN hPen = CreatePen(PS_SOLID, 2, color);
    HBRUSH hOldBrush = static_cast<HBRUSH>(SelectObject(g_hdcBack, hBrush));
    HPEN hOldPen = static_cast<HPEN>(SelectObject(g_hdcBack, hPen));
    
    ::Polygon(g_hdcBack, outPts, static_cast<int>(N));
    
    SelectObject(g_hdcBack, hOldBrush);
    SelectObject(g_hdcBack, hOldPen);
    DeleteObject(hBrush);
    DeleteObject(hPen);
    
    SetROP2(g_hdcBack, oldRop);
}

// Draw labels in columns on left/right sides of screen
template<size_t N>
static void DrawLabelsInColumn(
    const std::array<Point2F, N>& poly,
    const POINT* pts,
    const char* label,
    COLORREF color,
    bool isLeftSide,
    int columnX) noexcept
{
    // Create larger font for readability
    HFONT hFont = CreateFontA(
        20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Consolas");
    HFONT hOldFont = static_cast<HFONT>(SelectObject(g_hdcBack, hFont));
    
    HPEN hPen = CreatePen(PS_DOT, 1, color);
    HPEN hOldPen = static_cast<HPEN>(SelectObject(g_hdcBack, hPen));
    
    SetTextColor(g_hdcBack, color);
    SetBkMode(g_hdcBack, TRANSPARENT);
    
    char buf[32];
    
    // Spread labels across full screen height with padding
    const int topPadding = 50;
    const int bottomPadding = 50;
    const int availableHeight = g_height - topPadding - bottomPadding;
    const int lineHeight = availableHeight / static_cast<int>(N);
    
    for (size_t i = 0; i < N; ++i) {
        int labelY = topPadding + static_cast<int>(i) * lineHeight;
        
        // Format label text
        snprintf(buf, sizeof(buf), "%s%zu: (%.0f, %.0f)", label, i, poly[i].x, poly[i].y);
        int textLen = static_cast<int>(strlen(buf));
        
        // Calculate text width (approximate: 10 pixels per character for larger font)
        int textWidth = textLen * 10;
        
        int textX, lineStartX;
        if (isLeftSide) {
            textX = columnX;
            lineStartX = columnX + textWidth + 10;
        } else {
            textX = columnX;
            lineStartX = columnX - 10;
        }
        
        // Draw label text
        TextOutA(g_hdcBack, textX, labelY, buf, textLen);
        
        // Draw leader line from text to vertex
        MoveToEx(g_hdcBack, lineStartX, labelY + 10, nullptr);
        LineTo(g_hdcBack, pts[i].x, pts[i].y);
        
        // Draw vertex marker
        HBRUSH hMarker = CreateSolidBrush(color);
        HBRUSH hOldBrush = static_cast<HBRUSH>(SelectObject(g_hdcBack, hMarker));
        Ellipse(g_hdcBack, pts[i].x - 6, pts[i].y - 6, pts[i].x + 6, pts[i].y + 6);
        SelectObject(g_hdcBack, hOldBrush);
        DeleteObject(hMarker);
        
        // Draw index number at vertex
        char indexBuf[4];
        snprintf(indexBuf, sizeof(indexBuf), "%zu", i);
        SetTextColor(g_hdcBack, RGB(0, 0, 0));
        TextOutA(g_hdcBack, pts[i].x - 5, pts[i].y - 8, indexBuf, static_cast<int>(strlen(indexBuf)));
        SetTextColor(g_hdcBack, color);
    }
    
    SelectObject(g_hdcBack, hOldPen);
    DeleteObject(hPen);
    SelectObject(g_hdcBack, hOldFont);
    DeleteObject(hFont);
}

void DrawNT(float centerX, float centerY, float scale) noexcept {
    if (g_debugMode) {
        // Draw polygons with XOR mode in different colors
        POINT nPts[7], tPts[11];
        
        constexpr COLORREF nColor = RGB(255, 100, 100);  // Red-ish for N
        constexpr COLORREF tColor = RGB(100, 100, 255);  // Blue-ish for T
        
        DrawFilledPolygonDebugXOR(g_nPoly, scale, centerX, centerY, nColor, nPts);
        DrawFilledPolygonDebugXOR(g_tPoly, scale, centerX, centerY, tColor, tPts);
        
        // Draw labels - N on left (lines from right of text), T on right (lines from left of text)
        DrawLabelsInColumn(g_nPoly, nPts, "N", nColor, true, 10);
        DrawLabelsInColumn(g_tPoly, tPts, "T", tColor, false, g_width - 150);
        
        return;
    }

    float sin04, cos04, sin08, cos08, sin06, cos06;
    XMScalarSinCos(&sin04, &cos04, g_time * 0.4f);
    XMScalarSinCos(&sin08, &cos08, g_time * 0.8f);
    XMScalarSinCos(&sin06, &cos06, g_time * 0.6f);

    const float angleX = sin08 * 0.3f;
    const float angleY = g_time * 0.4f + sin06 * 0.2f;
    const float angleZ = g_time * 0.15f + sin04 * 0.15f;

    float sx, cx, sy, cy, sz, cz;
    XMScalarSinCos(&sx, &cx, angleX);
    XMScalarSinCos(&sy, &cy, angleY);
    XMScalarSinCos(&sz, &cz, angleZ);

    DrawFilledPolygon(g_nPoly, cx, cy, cz, sx, sy, sz, scale, centerX, centerY);
    DrawFilledPolygon(g_tPoly, cx, cy, cz, sx, sy, sz, scale, centerX, centerY);
}

void CreateBackBuffer(HWND hWnd) {
    RECT rc;
    GetClientRect(hWnd, &rc);
    g_width = rc.right;
    g_height = rc.bottom;
    
    if (g_width == 0 || g_height == 0) [[unlikely]] return;

    HDC hdc = GetDC(hWnd);
    g_hdcBack = CreateCompatibleDC(hdc);
    g_hbmBack = CreateCompatibleBitmap(hdc, g_width, g_height);
    g_hbmOld = static_cast<HBITMAP>(SelectObject(g_hdcBack, g_hbmBack));
    ReleaseDC(hWnd, hdc);
    
    g_hBgBrush = CreateSolidBrush(RGB(31, 31, 31));
    
    SetBkMode(g_hdcBack, TRANSPARENT);
    SelectObject(g_hdcBack, GetStockObject(NULL_PEN));
}

void DiscardBackBuffer() noexcept {
    if (g_hdcBack) {
        if (g_hbmOld) SelectObject(g_hdcBack, g_hbmOld);
        if (g_hbmBack) DeleteObject(g_hbmBack);
        DeleteDC(g_hdcBack);
        g_hdcBack = nullptr;
        g_hbmBack = nullptr;
        g_hbmOld = nullptr;
    }
    if (g_hBrush) { DeleteObject(g_hBrush); g_hBrush = nullptr; }
    if (g_hBgBrush) { DeleteObject(g_hBgBrush); g_hBgBrush = nullptr; }
}

void Render(HWND hWnd) noexcept {
    if (!g_hdcBack) [[unlikely]] return;
    
    const RECT rc = { 0, 0, g_width, g_height };
    FillRect(g_hdcBack, &rc, g_hBgBrush);

    BYTE r, g, b;
    if (g_debugMode) {
        r = 80; g = 80; b = 80;  // Dark gray base for XOR visibility
    } else {
        const float progress = g_colorProgress;
        r = static_cast<BYTE>((g_currentR + (g_targetR - g_currentR) * progress) * 255.0f);
        g = static_cast<BYTE>((g_currentG + (g_targetG - g_currentG) * progress) * 255.0f);
        b = static_cast<BYTE>((g_currentB + (g_targetB - g_currentB) * progress) * 255.0f);
    }

    if (g_hBrush) DeleteObject(g_hBrush);
    g_hBrush = CreateSolidBrush(RGB(r, g, b));
    SelectObject(g_hdcBack, g_hBrush);
    
    const float scale = std::min(static_cast<float>(g_width), static_cast<float>(g_height)) * INV_BASE_WIDTH;
    DrawNT(g_width * 0.5f, g_height * 0.5f, scale);

    if (g_debugMode) {
        SetTextColor(g_hdcBack, RGB(255, 255, 0));
        const char* msg = "DEBUG MODE - Q: exit | Overlap shows as different color";
        TextOutA(g_hdcBack, 10, 10, msg, static_cast<int>(strlen(msg)));
    }

    HDC hdc = GetDC(hWnd);
    BitBlt(hdc, 0, 0, g_width, g_height, g_hdcBack, 0, 0, SRCCOPY);
    ReleaseDC(hWnd, hdc);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        RandomColorF(g_currentR, g_currentG, g_currentB);
        RandomColorF(g_targetR, g_targetG, g_targetB);
        QueryPerformanceFrequency(&g_perfFreq);
        QueryPerformanceCounter(&g_lastTime);
        CreateBackBuffer(hWnd);
        return 0;

    case WM_SIZE:
        DiscardBackBuffer();
        CreateBackBuffer(hWnd);
        return 0;

    case WM_KEYDOWN:
        if (wParam == 'Q') {
            g_debugMode = !g_debugMode;
            if (g_debugMode) {
                ToggleFullScreen(hWnd);  // Go full screen when entering debug
            } else if (g_isFullScreen) {
                ToggleFullScreen(hWnd);  // Exit full screen when leaving debug
            }
            InvalidateRect(hWnd, nullptr, FALSE);
        }
        if (wParam == VK_ESCAPE && g_isFullScreen) {
            ToggleFullScreen(hWnd);
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hWnd, &ps);
        Render(hWnd);
        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        g_isRunning = false;
        DiscardBackBuffer();
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}

int APIENTRY WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int nCmdShow) {
    WNDCLASSEXW wc = { sizeof(wc), CS_HREDRAW | CS_VREDRAW | CS_OWNDC, WndProc, 0, 0, hInstance,
                       nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"NTWobble", nullptr };
    RegisterClassExW(&wc);

    HWND hWnd = CreateWindowExW(0, L"NTWobble", L"NT Wobble", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 600, 500, nullptr, nullptr, hInstance, nullptr);
    if (!hWnd) return 0;

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg = {};
    while (g_isRunning) {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_isRunning = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (!g_isRunning) break;

        if (!g_debugMode) {
            LARGE_INTEGER currentTime;
            QueryPerformanceCounter(&currentTime);
            const float deltaTime = static_cast<float>(currentTime.QuadPart - g_lastTime.QuadPart) 
                                  / static_cast<float>(g_perfFreq.QuadPart);
            g_lastTime = currentTime;

            g_time += deltaTime * 3.0f;
            if (g_time > TWO_PI * 1000.0f) [[unlikely]]
                g_time -= TWO_PI * 1000.0f;
            
            g_colorProgress += deltaTime * 0.3f;

            if (g_colorProgress >= 1.0f) [[unlikely]] {
                g_colorProgress = 0.0f;
                g_currentR = g_targetR;
                g_currentG = g_targetG;
                g_currentB = g_targetB;
                RandomColorF(g_targetR, g_targetG, g_targetB);
            }
        }

        Render(hWnd);
    }

    return static_cast<int>(msg.wParam);
}

