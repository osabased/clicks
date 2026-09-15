#ifdef _WIN32

#define GLFW_EXPOSE_NATIVE_WIN32

#include <Geode/Geode.hpp>
#include <glfw/glfw3.h>
#include <glfw/glfw3native.h>

#include <windows.h>

using namespace geode::prelude;

namespace {

struct WindowLookup {
    GLFWwindow* glfwWindow = nullptr;
    HWND hwnd = nullptr;
};

BOOL CALLBACK findGlfwHwnd(HWND hwnd, LPARAM param) {
    auto* lookup = reinterpret_cast<WindowLookup*>(param);

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId != GetCurrentProcessId()) {
        return TRUE;
    }

    // GLFW 3.1's Win32 backend stores its _GLFWwindow* in window-extra slot 0
    // during WM_NCCREATE. Geode ships those GLFW 3.1 headers but does not link
    // the native-accessor symbols, so recover the same HWND without introducing
    // a second GLFW runtime or searching by title/class name.
    auto const storedWindow = reinterpret_cast<GLFWwindow*>(GetWindowLongPtrW(hwnd, 0));
    if (storedWindow != lookup->glfwWindow) {
        return TRUE;
    }

    lookup->hwnd = hwnd;
    return FALSE;
}

} // namespace

extern "C" HWND glfwGetWin32Window(GLFWwindow* window) {
    if (!window) {
        return nullptr;
    }

    WindowLookup lookup{window, nullptr};
    EnumWindows(findGlfwHwnd, reinterpret_cast<LPARAM>(&lookup));
    return lookup.hwnd;
}

extern "C" GLFWmonitor* glfwGetWindowMonitor(GLFWwindow* window) {
    if (!window) {
        return nullptr;
    }

    auto* view = CCEGLView::get();
    if (!view || view->getWindow() != window || !view->getIsFullscreen()) {
        return nullptr;
    }

    // Callers in this prototype only test nullness. Do not expose or dereference
    // a fabricated monitor object; this bridge exists solely because the native
    // GLFW symbols are absent from Geode's Windows link set.
    return reinterpret_cast<GLFWmonitor*>(static_cast<uintptr_t>(1));
}

#endif
