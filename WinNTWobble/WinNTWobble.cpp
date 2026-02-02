// NTWobble.cpp : GDI rotating NT logo with color transitions

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h> 
#pragma comment(lib, "dwmapi.lib")
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

// Helper macro for extracting x from lParam in mouse messages
#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#endif

// Helper macro for extracting y from lParam in mouse messages
#ifndef GET_Y_LPARAM
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif

// GDI resources
HDC g_hdcBack = nullptr;
HDC g_hdcWindow = nullptr;  // Cached window DC (CS_OWNDC)
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
int g_mouseStartX = -1;
int g_mouseStartY = -1;

std::mt19937 g_rng{std::random_device{}()};
std::uniform_real_distribution<float> g_colorDist{0.0f, 1.0f};

// SoA (Structure of Arrays) layout for SIMD-friendly access
// Padded to 20 vertices (divisible by 4) - last 2 are duplicates for degenerate handling
constexpr size_t POLY_VERTEX_COUNT = 18;  // Actual vertices for GDI
constexpr size_t POLY_SIMD_COUNT = 20;    // Padded for SIMD (5 batches of 4)

alignas(16) constexpr float g_ntPolyX[POLY_SIMD_COUNT] = {
    -146, -110,  -26,  -26,   // batch 0
     146,  146,   97,   97,   // batch 1
      97,   60,   60,    9,   // batch 2
       9,    9,  -27, -110,   // batch 3
    -110, -146, -146, -146    // batch 4 (last 2 are padding duplicates)
};

alignas(16) constexpr float g_ntPolyY[POLY_SIMD_COUNT] = {
     -93,  -93,   32,  -93,   // batch 0
     -93,  -57,  -57,   57,   // batch 1
      93,   93,  -57,  -57,   // batch 2
      57,   93,   93,  -32,   // batch 3
      93,   93,   93,   93    // batch 4 (last 2 are padding duplicates)
};

inline void RandomColorF(float& r, float& g, float& b) noexcept {
    r = g_colorDist(g_rng);
    g = g_colorDist(g_rng);
    b = g_colorDist(g_rng);
}

// SIMD-optimized polygon transformation using SoA layout
static void DrawFilledPolygon(
    float cx, float cy, float cz,
    float sx, float sy, float sz,
    float scale, float centerX, float centerY) noexcept
{
    alignas(16) POINT pts[POLY_SIMD_COUNT];
    
    // Broadcast constants to SIMD registers
    const XMVECTOR vCx = XMVectorReplicate(cx);
    const XMVECTOR vCy = XMVectorReplicate(cy);
    const XMVECTOR vCz = XMVectorReplicate(cz);
    const XMVECTOR vSx = XMVectorReplicate(sx);
    const XMVECTOR vSy = XMVectorReplicate(sy);
    const XMVECTOR vSz = XMVectorReplicate(sz);
    const XMVECTOR vScale = XMVectorReplicate(scale);
    const XMVECTOR vCenterX = XMVectorReplicate(centerX);
    const XMVECTOR vCenterY = XMVectorReplicate(centerY);
    const XMVECTOR vPerspDist = XMVectorReplicate(PERSPECTIVE_DIST);
    
    // Process all 20 vertices in 5 batches of 4 (no remainder loop needed)
    for (size_t i = 0; i < POLY_SIMD_COUNT; i += 4) {
        // Aligned SIMD load from SoA arrays (much faster than XMVectorSet)
        const XMVECTOR vPx = XMLoadFloat4(reinterpret_cast<const XMFLOAT4*>(&g_ntPolyX[i]));
        const XMVECTOR vPy = XMLoadFloat4(reinterpret_cast<const XMFLOAT4*>(&g_ntPolyY[i]));
        
        // Transform: y = py * cx, z1 = py * sx
        const XMVECTOR vY = XMVectorMultiply(vPy, vCx);
        const XMVECTOR vZ1 = XMVectorMultiply(vPy, vSx);
        
        // x = px * cy + z1 * sy
        const XMVECTOR vX = XMVectorMultiplyAdd(vZ1, vSy, XMVectorMultiply(vPx, vCy));
        
        // z = z1 * cy - px * sy
        const XMVECTOR vZ = XMVectorSubtract(XMVectorMultiply(vZ1, vCy), XMVectorMultiply(vPx, vSy));
        
        // nx = x * cz - y * sz, ny = x * sz + y * cz
        const XMVECTOR vNx = XMVectorSubtract(XMVectorMultiply(vX, vCz), XMVectorMultiply(vY, vSz));
        const XMVECTOR vNy = XMVectorMultiplyAdd(vX, vSz, XMVectorMultiply(vY, vCz));
        
        // Perspective: invZ = 1 / (PERSPECTIVE_DIST + z), s = PERSPECTIVE_DIST * invZ * scale
        const XMVECTOR vInvZ = XMVectorReciprocal(XMVectorAdd(vPerspDist, vZ));
        const XMVECTOR vS = XMVectorMultiply(XMVectorMultiply(vPerspDist, vInvZ), vScale);
        
        // Final position: result = n * s + center
            const XMVECTOR vResultX = XMVectorMultiplyAdd(vNx, vS, vCenterX);
            const XMVECTOR vResultY = XMVectorMultiplyAdd(vNy, vS, vCenterY);
        
            // Convert to integers and store
            const XMVECTOR vIntX = XMVectorTruncate(vResultX);
            const XMVECTOR vIntY = XMVectorTruncate(vResultY);
        
            // Store results to temp arrays
            alignas(16) float tempX[4], tempY[4];
            XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(tempX), vIntX);
            XMStoreFloat4(reinterpret_cast<XMFLOAT4*>(tempY), vIntY);
        
            pts[i].x = static_cast<LONG>(tempX[0]);
            pts[i].y = static_cast<LONG>(tempY[0]);
            pts[i+1].x = static_cast<LONG>(tempX[1]);
            pts[i+1].y = static_cast<LONG>(tempY[1]);
            pts[i+2].x = static_cast<LONG>(tempX[2]);
            pts[i+2].y = static_cast<LONG>(tempY[2]);
            pts[i+3].x = static_cast<LONG>(tempX[3]);
            pts[i+3].y = static_cast<LONG>(tempY[3]);
        }

    // Draw only the actual 18 vertices (ignore padding)
    ::Polygon(g_hdcBack, pts, static_cast<int>(POLY_VERTEX_COUNT));
}

void DrawNT(float centerX, float centerY, float scale) noexcept {
    // Pre-calculate time multiplier (used twice)
    const float time04 = g_time * 0.4f;
    
    // Batch first set using SIMD - only need sin values, not cos
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
    
        // Select background brush for PatBlt operations
        SelectObject(g_hdcBack, g_hBgBrush);
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

void Render() noexcept {
if (!g_hdcBack) [[unlikely]] return;
    
    // Clear background using PatBlt (faster than FillRect - uses selected brush)
    PatBlt(g_hdcBack, 0, 0, g_width, g_height, PATCOPY);

    const auto progress = g_colorProgress;
    const BYTE r = LerpToByte(g_currentR, g_targetR, progress);
    const BYTE g = LerpToByte(g_currentG, g_targetG, progress);
    const BYTE b = LerpToByte(g_currentB, g_targetB, progress);

    // Only recreate brush when color actually changes
    if (r != g_lastR || g != g_lastG || b != g_lastB) {
        g_lastR = r; g_lastG = g; g_lastB = b;
        if (g_hBrush) DeleteObject(g_hBrush);
        g_hBrush = CreateSolidBrush(RGB(r, g, b));
    }
    // Always select fill brush before drawing (PatBlt uses bg brush, polygon needs fill brush)
    SelectObject(g_hdcBack, g_hBrush);
    
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

        // Re-select background brush for next frame's PatBlt
        SelectObject(g_hdcBack, g_hBgBrush);

        // Use cached window DC (CS_OWNDC) - no GetDC/ReleaseDC overhead
        BitBlt(g_hdcWindow, 0, 0, g_width, g_height, g_hdcBack, 0, 0, SRCCOPY);
    }

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE:
        RandomColorF(g_currentR, g_currentG, g_currentB);
        RandomColorF(g_targetR, g_targetG, g_targetB);
        QueryPerformanceFrequency(&g_perfFreq);
        QueryPerformanceCounter(&g_lastTime);
        CreateBackBuffer(hWnd);
        // Initialize mouse baseline
        POINT pt; GetCursorPos(&pt);
        g_mouseStartX = pt.x; g_mouseStartY = pt.y;
        return 0;

    case WM_SIZE:
        DiscardBackBuffer();
        CreateBackBuffer(hWnd);
        return 0;

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        // Any key exits screensaver; keep 'B' toggle when running as app
        if (wParam == 'B') {
            g_showBorder = !g_showBorder;
        }
        g_isRunning = false;
        PostQuitMessage(0);
        return 0;

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        g_isRunning = false;
        PostQuitMessage(0);
        return 0;

    case WM_MOUSEMOVE: {
        // Exit if mouse moved significantly from initial position
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        if (g_mouseStartX >= 0 && g_mouseStartY >= 0) {
            if (std::abs(x - g_mouseStartX) > 8 || std::abs(y - g_mouseStartY) > 8) {
                g_isRunning = false;
                PostQuitMessage(0);
            }
        } else {
            g_mouseStartX = x; g_mouseStartY = y;
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hWnd, &ps);
        Render();
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

    // Create borderless fullscreen window suitable for screensaver
    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    HWND hWnd = CreateWindowExW(WS_EX_TOPMOST, L"NTWobble", L"NT Wobble", WS_POPUP,
        0, 0, screenWidth, screenHeight, nullptr, nullptr, hInstance, nullptr);
    if (!hWnd) [[unlikely]] return 0;

    // Cache window DC (CS_OWNDC ensures it's private and persistent)
    g_hdcWindow = GetDC(hWnd);

    // Hide cursor for screensaver mode
    ShowCursor(FALSE);

    // Show fullscreen window
    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    MSG msg {};
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

        Render();
        DwmFlush();
    }

    // Restore cursor visibility before exit
    ShowCursor(TRUE);
    return static_cast<int>(msg.wParam);
}
