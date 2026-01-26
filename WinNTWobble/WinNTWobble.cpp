// NTWobble.cpp : GDI rotating NT logo with color transitions

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cmath>
#include <random>
#include <array>
#include <algorithm>
#include <numbers>
#include <utility>
#include <DirectXMath.h>

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
HPEN g_hBorderPen = nullptr;
int g_width = 0, g_height = 0;

// Cached GDI state to avoid unnecessary recreation
BYTE g_lastR = 0, g_lastG = 0, g_lastB = 0;
int g_lastPenWidth = 0;

// Animation state
float g_time = 0.0f;
float g_currentR, g_currentG, g_currentB;
float g_targetR, g_targetG, g_targetB;
float g_colorProgress = 0.0f;
LARGE_INTEGER g_perfFreq, g_lastTime;
bool g_isRunning = true;
bool g_showBorder = true;

std::mt19937 g_rng{std::random_device{}()};
std::uniform_real_distribution<float> g_colorDist{0.0f, 1.0f};

struct Point2F { float x, y; };

constexpr auto g_ntPoly = std::to_array<Point2F>({
    {-146, -93}, {-110, -93}, { -26,  32}, { -26, -93},
    { 146, -93}, { 146, -57}, {  97, -57}, {  97,  57},
    {  97,  93}, {  60,  93}, {  60, -57}, {   9, -57},
    {   9,  57}, {   9,  93}, { -27,  93}, {-110, -32},
    {-110,  93}, {-146,  93},
});

inline void RandomColorF(float& r, float& g, float& b) noexcept {
    r = g_colorDist(g_rng);
    g = g_colorDist(g_rng);
    b = g_colorDist(g_rng);
}

inline POINT TransformPoint(float px, float py, float cx, float cy, float cz,
    float sx, float sy, float sz, float scale, float centerX, float centerY) noexcept {
    [[assume(scale > 0.0f)]];
    [[assume(PERSPECTIVE_DIST > 0.0f)]];
    
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

static void DrawFilledPolygon(
    float cx, float cy, float cz,
    float sx, float sy, float sz,
    float scale, float centerX, float centerY) noexcept
{
    constexpr auto N = g_ntPoly.size();
    POINT pts[N];
    
    for (auto i = 0uz; i < N; ++i) {
        pts[i] = TransformPoint(g_ntPoly[i].x, g_ntPoly[i].y, 
                                cx, cy, cz, sx, sy, sz, scale, centerX, centerY);
    }

    ::Polygon(g_hdcBack, pts, static_cast<int>(N));
}

void DrawNT(float centerX, float centerY, float scale) noexcept {
    // Pre-calculate time multiplier (used twice)
    const float time04 = g_time * 0.4f;
    
    // Batch first set using SIMD - only need sin values, not cos (eliminates 3 unused cos calculations)
    const XMVECTOR timeAngles = XMVectorSet(time04, g_time * 0.8f, g_time * 0.6f, 0.0f);
    const XMVECTOR sinVec1 = XMVectorSin(timeAngles);
    
    const float sin04 = XMVectorGetX(sinVec1);
    const float sin08 = XMVectorGetY(sinVec1);
    const float sin06 = XMVectorGetZ(sinVec1);

    // Calculate rotation angles
    const float angleX = sin08 * 0.3f;
    const float angleY = time04 + sin06 * 0.2f;
    const float angleZ = g_time * 0.15f + sin04 * 0.15f;

    // Batch second set using SIMD (3 scalar calls -> 1 vector call)
    const XMVECTOR rotAngles = XMVectorSet(angleX, angleY, angleZ, 0.0f);
    XMVECTOR sinVec2, cosVec2;
    XMVectorSinCos(&sinVec2, &cosVec2, rotAngles);
    
    const float sx = XMVectorGetX(sinVec2);
    const float cx = XMVectorGetX(cosVec2);
    const float sy = XMVectorGetY(sinVec2);
    const float cy = XMVectorGetY(cosVec2);
    const float sz = XMVectorGetZ(sinVec2);
    const float cz = XMVectorGetZ(cosVec2);

    DrawFilledPolygon(cx, cy, cz, sx, sy, sz, scale, centerX, centerY);
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
    if (g_hBorderPen) { DeleteObject(g_hBorderPen); g_hBorderPen = nullptr; }
}

[[nodiscard]] constexpr BYTE LerpToByte(float current, float target, float t) noexcept {
    return static_cast<BYTE>(std::lerp(current, target, t) * 255.0f);
}

void Render(HWND hWnd) noexcept {
    if (!g_hdcBack) [[unlikely]] return;
    
    const RECT rc = { 0, 0, g_width, g_height };
    FillRect(g_hdcBack, &rc, g_hBgBrush);

    const auto progress = g_colorProgress;
    const BYTE r = LerpToByte(g_currentR, g_targetR, progress);
    const BYTE g = LerpToByte(g_currentG, g_targetG, progress);
    const BYTE b = LerpToByte(g_currentB, g_targetB, progress);

    // Only recreate brush when color actually changes
    if (r != g_lastR || g != g_lastG || b != g_lastB) {
        g_lastR = r; g_lastG = g; g_lastB = b;
        if (g_hBrush) DeleteObject(g_hBrush);
        g_hBrush = CreateSolidBrush(RGB(r, g, b));
        SelectObject(g_hdcBack, g_hBrush);
    }
    
    const float scale = std::min(static_cast<float>(g_width), static_cast<float>(g_height)) * INV_BASE_WIDTH;
    
    // Scale border thickness with polygon size (min 1px, scales with window)
    if (g_showBorder) {
        const int penWidth = std::max(1, static_cast<int>(scale * 1.5f));
        // Only recreate pen when width changes (typically on resize)
        if (penWidth != g_lastPenWidth) {
            g_lastPenWidth = penWidth;
            if (g_hBorderPen) DeleteObject(g_hBorderPen);
            g_hBorderPen = CreatePen(PS_SOLID, penWidth, RGB(255, 255, 255));
            SelectObject(g_hdcBack, g_hBorderPen);
        }
    } else {
        if (g_lastPenWidth != 0) {
            g_lastPenWidth = 0;
            SelectObject(g_hdcBack, GetStockObject(NULL_PEN));
        }
    }
    
    DrawNT(g_width * 0.5f, g_height * 0.5f, scale);

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
        if (wParam == 'B') {
            g_showBorder = !g_showBorder;
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
    if (!hWnd) [[unlikely]] return 0;

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg = {};
    while (g_isRunning) [[likely]] {
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) [[unlikely]] {
                g_isRunning = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (!g_isRunning) [[unlikely]] break;

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

        Render(hWnd);
    }

    return static_cast<int>(msg.wParam);
}
