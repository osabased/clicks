#ifdef _WIN32

#define GLFW_EXPOSE_NATIVE_WIN32

#include "streamproof_overlay.hpp"

#include <Geode/Geode.hpp>

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdlib>
#include <memory>
#include <string_view>

using Microsoft::WRL::ComPtr;
using namespace geode::prelude;

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace streamproof {
namespace {

constexpr wchar_t kOverlayClassName[] = L"ClickIndicators.Streamproof.GateA";
constexpr DWORD kMinimumWindowsBuild = 19041;

bool envFlag(char const* name) {
    if (auto const* value = std::getenv(name)) {
        std::string_view text(value);
        return !text.empty() && text != "0" && text != "false" && text != "FALSE";
    }
    return false;
}

LRESULT CALLBACK overlayWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCHITTEST) {
        return HTTRANSPARENT;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool isSupportedWindowsBuild(DWORD& buildNumber) {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto const ntdll = GetModuleHandleW(L"ntdll.dll");
    auto const rtlGetVersion = ntdll
        ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"))
        : nullptr;
    if (!rtlGetVersion) {
        return false;
    }

    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (rtlGetVersion(&version) != 0) {
        return false;
    }

    buildNumber = version.dwBuildNumber;
    return version.dwMajorVersion >= 10 && version.dwBuildNumber >= kMinimumWindowsBuild;
}

bool isWineOrProton() {
    auto const ntdll = GetModuleHandleW(L"ntdll.dll");
    return ntdll && GetProcAddress(ntdll, "wine_get_version") != nullptr;
}

bool isCompositionAvailable() {
    BOOL enabled = FALSE;
    return SUCCEEDED(DwmIsCompositionEnabled(&enabled)) && enabled == TRUE;
}

bool validateGameWindow(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) {
        return false;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId != GetCurrentProcessId()) {
        return false;
    }

    if (GetAncestor(hwnd, GA_ROOT) != hwnd) {
        return false;
    }

    auto const style = static_cast<DWORD_PTR>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    return (style & WS_CHILD) == 0;
}

} // namespace

struct StreamproofOverlay::Impl {
    HWND game = nullptr;
    HWND overlay = nullptr;
    bool affinityReady = false;
    bool graphicsReady = false;
    bool visible = false;

    ComPtr<ID3D11Device> d3dDevice;
    ComPtr<ID3D11DeviceContext> d3dContext;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> d2dContext;
    ComPtr<ID2D1Bitmap1> d2dTarget;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<IDCompositionDevice> compositionDevice;
    ComPtr<IDCompositionTarget> compositionTarget;
    ComPtr<IDCompositionVisual> compositionVisual;

    void fail(char const* reason) {
        log::error("Streamproof Gate A overlay disabled: {}", reason);
        hide();
    }

    void hide() {
        visible = false;
        if (overlay && IsWindow(overlay)) {
            ShowWindow(overlay, SW_HIDE);
        }
    }

    void resetGraphics() {
        compositionVisual.Reset();
        compositionTarget.Reset();
        compositionDevice.Reset();
        brush.Reset();
        d2dTarget.Reset();
        d2dContext.Reset();
        d2dDevice.Reset();
        d2dFactory.Reset();
        swapChain.Reset();
        d3dContext.Reset();
        d3dDevice.Reset();
        graphicsReady = false;
    }

    void destroyWindow() {
        hide();
        resetGraphics();
        affinityReady = false;
        if (overlay && IsWindow(overlay)) {
            DestroyWindow(overlay);
        }
        overlay = nullptr;
        game = nullptr;
    }

    bool registerOverlayClass() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = overlayWindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kOverlayClassName;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);

        if (RegisterClassExW(&wc) != 0) {
            return true;
        }
        return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    GLFWwindow* acquireGlfwWindow(HWND& hwnd) {
        auto* view = CCEGLView::get();
        auto* glfwWindow = view ? view->getWindow() : nullptr;
        hwnd = glfwWindow ? glfwGetWin32Window(glfwWindow) : nullptr;

        auto const dc = wglGetCurrentDC();
        auto const fromDc = dc ? WindowFromDC(dc) : nullptr;
        if (hwnd && fromDc && hwnd != fromDc) {
            log::warn("Streamproof Gate A: GLFW HWND and current-DC HWND disagree; rejecting startup");
            hwnd = nullptr;
            return nullptr;
        }

        if (!hwnd && fromDc) {
            log::warn("Streamproof Gate A: GLFW native HWND unavailable; using current-DC fallback");
            hwnd = fromDc;
        }

        return glfwWindow;
    }

    bool alignToGameClient(RECT& clientRect, UINT& width, UINT& height) {
        if (!game || !GetClientRect(game, &clientRect)) {
            return false;
        }

        POINT origin{clientRect.left, clientRect.top};
        if (!ClientToScreen(game, &origin)) {
            return false;
        }

        width = static_cast<UINT>(clientRect.right - clientRect.left);
        height = static_cast<UINT>(clientRect.bottom - clientRect.top);
        if (width == 0 || height == 0) {
            return false;
        }

        return SetWindowPos(
            overlay,
            game,
            origin.x,
            origin.y,
            static_cast<int>(width),
            static_cast<int>(height),
            SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_HIDEWINDOW
        ) != FALSE;
    }

    bool createHiddenWindow(UINT& width, UINT& height) {
        if (!registerOverlayClass()) {
            fail("unable to register overlay window class");
            return false;
        }

        constexpr DWORD exStyle = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP;
        overlay = CreateWindowExW(
            exStyle,
            kOverlayClassName,
            L"Click Indicators Streamproof Gate A",
            WS_POPUP,
            0,
            0,
            1,
            1,
            game,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr
        );
        if (!overlay) {
            fail("unable to create hidden overlay HWND");
            return false;
        }

        if (IsWindowVisible(overlay)) {
            fail("overlay HWND unexpectedly became visible during creation");
            return false;
        }

        if (GetWindow(overlay, GW_OWNER) != game) {
            fail("overlay HWND is not owned by the validated Geometry Dash window");
            return false;
        }

        RECT rect{};
        if (!alignToGameClient(rect, width, height)) {
            fail("unable to align hidden overlay to the Geometry Dash client area");
            return false;
        }

        return true;
    }

    bool applyCaptureProtection() {
        if (envFlag("CLICK_INDICATORS_STREAMPROOF_FORCE_AFFINITY_FAILURE")) {
            fail("forced display-affinity failure");
            return false;
        }

        if (!SetWindowDisplayAffinity(overlay, WDA_EXCLUDEFROMCAPTURE)) {
            fail("SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) failed");
            return false;
        }

        affinityReady = true;
        return true;
    }

    bool initializeGraphics(UINT width, UINT height) {
        if (envFlag("CLICK_INDICATORS_STREAMPROOF_FORCE_GRAPHICS_FAILURE")) {
            fail("forced graphics initialization failure");
            return false;
        }

        UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifndef NDEBUG
        deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        D3D_FEATURE_LEVEL featureLevel{};
        auto hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            deviceFlags,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &d3dDevice,
            &featureLevel,
            &d3dContext
        );
#ifndef NDEBUG
        if (FAILED(hr)) {
            deviceFlags &= ~D3D11_CREATE_DEVICE_DEBUG;
            hr = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                deviceFlags,
                nullptr,
                0,
                D3D11_SDK_VERSION,
                &d3dDevice,
                &featureLevel,
                &d3dContext
            );
        }
#endif
        if (FAILED(hr)) {
            fail("D3D11 device creation failed");
            return false;
        }

        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(d3dDevice.As(&dxgiDevice))) {
            fail("D3D11 device does not expose IDXGIDevice");
            return false;
        }

        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDevice->GetAdapter(&adapter))) {
            fail("unable to obtain DXGI adapter");
            return false;
        }

        ComPtr<IDXGIFactory2> factory;
        if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
            fail("unable to obtain IDXGIFactory2");
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 swapDesc{};
        swapDesc.Width = width;
        swapDesc.Height = height;
        swapDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        swapDesc.SampleDesc.Count = 1;
        swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDesc.BufferCount = 2;
        swapDesc.Scaling = DXGI_SCALING_STRETCH;
        swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        swapDesc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

        if (FAILED(factory->CreateSwapChainForComposition(
                d3dDevice.Get(), &swapDesc, nullptr, &swapChain))) {
            fail("CreateSwapChainForComposition failed");
            return false;
        }

        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2dFactory))) {
            fail("D2D1 factory creation failed");
            return false;
        }
        if (FAILED(d2dFactory->CreateDevice(dxgiDevice.Get(), &d2dDevice))) {
            fail("D2D1 device creation failed");
            return false;
        }
        if (FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dContext))) {
            fail("D2D1 device-context creation failed");
            return false;
        }

        ComPtr<IDXGISurface> surface;
        if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&surface)))) {
            fail("unable to obtain composition swap-chain surface");
            return false;
        }

        auto const bitmapProperties = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f,
            96.0f
        );
        if (FAILED(d2dContext->CreateBitmapFromDxgiSurface(
                surface.Get(), &bitmapProperties, &d2dTarget))) {
            fail("unable to create D2D target bitmap");
            return false;
        }
        d2dContext->SetTarget(d2dTarget.Get());

        if (FAILED(d2dContext->CreateSolidColorBrush(
                D2D1::ColorF(D2D1::ColorF::DeepSkyBlue, 0.90f), &brush))) {
            fail("unable to create D2D test brush");
            return false;
        }

        if (FAILED(DCompositionCreateDevice(
                dxgiDevice.Get(), IID_PPV_ARGS(&compositionDevice)))) {
            fail("DirectComposition device creation failed");
            return false;
        }
        if (FAILED(compositionDevice->CreateTargetForHwnd(
                overlay, TRUE, &compositionTarget))) {
            fail("DirectComposition HWND target creation failed");
            return false;
        }
        if (FAILED(compositionDevice->CreateVisual(&compositionVisual))) {
            fail("DirectComposition visual creation failed");
            return false;
        }
        if (FAILED(compositionVisual->SetContent(swapChain.Get()))) {
            fail("DirectComposition visual content attachment failed");
            return false;
        }
        if (FAILED(compositionTarget->SetRoot(compositionVisual.Get()))) {
            fail("DirectComposition root attachment failed");
            return false;
        }
        if (FAILED(compositionDevice->Commit())) {
            fail("DirectComposition commit failed");
            return false;
        }

        graphicsReady = true;
        return true;
    }

    bool renderTestFrame(UINT width, UINT height) {
        if (!graphicsReady) {
            fail("attempted test rendering without initialized graphics");
            return false;
        }

        d2dContext->BeginDraw();
        d2dContext->Clear(D2D1::ColorF(0.0f, 0.0f));

        auto const inset = 24.0f;
        auto const right = static_cast<float>(width) - inset;
        auto const bottom = static_cast<float>(height) - inset;
        if (right > inset && bottom > inset) {
            d2dContext->DrawRectangle(
                D2D1::RectF(inset, inset, right, bottom),
                brush.Get(),
                4.0f
            );
            d2dContext->FillEllipse(
                D2D1::Ellipse(
                    D2D1::Point2F(static_cast<float>(width) * 0.5f, 48.0f),
                    12.0f,
                    12.0f
                ),
                brush.Get()
            );
        }

        if (FAILED(d2dContext->EndDraw())) {
            fail("D2D test-frame rendering failed");
            return false;
        }
        if (FAILED(swapChain->Present(1, 0))) {
            fail("composition swap-chain Present failed");
            return false;
        }

        return true;
    }

    bool showProtected() {
        if (!overlay || !affinityReady || !graphicsReady) {
            fail("visibility requested before mandatory protection and graphics initialization");
            return false;
        }

        ShowWindow(overlay, SW_SHOWNOACTIVATE);
        if (!IsWindowVisible(overlay)) {
            fail("overlay failed to become visible after mandatory initialization");
            return false;
        }

        visible = true;
        return true;
    }
};

StreamproofOverlay& StreamproofOverlay::get() {
    static StreamproofOverlay instance;
    return instance;
}

StreamproofOverlay::~StreamproofOverlay() {
    shutdown();
}

bool StreamproofOverlay::initialize() {
    if (!m_impl) {
        m_impl = new Impl();
    }
    m_impl->destroyWindow();

    DWORD buildNumber = 0;
    if (!isSupportedWindowsBuild(buildNumber)) {
        m_impl->fail("Windows 10 build 19041 or newer is required");
        return false;
    }
    if (isWineOrProton()) {
        m_impl->fail("Wine/Proton is not validated for the Streamproof Gate A prototype");
        return false;
    }
    if (!isCompositionAvailable()) {
        m_impl->fail("Desktop Window Manager composition is unavailable");
        return false;
    }

    HWND gameHwnd = nullptr;
    auto* glfwWindow = m_impl->acquireGlfwWindow(gameHwnd);
    if (!validateGameWindow(gameHwnd)) {
        m_impl->fail("unable to acquire a valid current-process top-level Geometry Dash HWND");
        return false;
    }
    if (glfwWindow && glfwGetWindowMonitor(glfwWindow) != nullptr) {
        m_impl->fail("exclusive GLFW fullscreen is unsupported by the Gate A prototype");
        return false;
    }

    m_impl->game = gameHwnd;

    UINT width = 0;
    UINT height = 0;
    if (!m_impl->createHiddenWindow(width, height)) {
        m_impl->destroyWindow();
        return false;
    }
    if (!m_impl->applyCaptureProtection()) {
        m_impl->destroyWindow();
        return false;
    }
    if (!m_impl->initializeGraphics(width, height)) {
        m_impl->destroyWindow();
        return false;
    }
    if (!m_impl->renderTestFrame(width, height)) {
        m_impl->destroyWindow();
        return false;
    }
    if (!m_impl->showProtected()) {
        m_impl->destroyWindow();
        return false;
    }

    log::info(
        "Streamproof Gate A overlay prototype visible on Windows build {}; capture behavior is not yet validated",
        buildNumber
    );
    return true;
}

void StreamproofOverlay::shutdown() {
    if (!m_impl) {
        return;
    }
    m_impl->destroyWindow();
    delete m_impl;
    m_impl = nullptr;
}

bool StreamproofOverlay::isVisible() const {
    return m_impl && m_impl->visible && m_impl->overlay && IsWindowVisible(m_impl->overlay);
}

HWND StreamproofOverlay::overlayWindow() const {
    return m_impl ? m_impl->overlay : nullptr;
}

HWND StreamproofOverlay::gameWindow() const {
    return m_impl ? m_impl->game : nullptr;
}

$on_mod(Loaded) {
    StreamproofOverlay::get().initialize();
}

} // namespace streamproof

#endif
