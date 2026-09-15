#pragma once

#ifdef _WIN32

#include <windows.h>

namespace streamproof {

class StreamproofOverlay final {
public:
    static StreamproofOverlay& get();

    bool initialize();
    void shutdown();

    bool isVisible() const;
    HWND overlayWindow() const;
    HWND gameWindow() const;

private:
    StreamproofOverlay() = default;
    ~StreamproofOverlay();

    StreamproofOverlay(StreamproofOverlay const&) = delete;
    StreamproofOverlay& operator=(StreamproofOverlay const&) = delete;

    struct Impl;
    Impl* m_impl = nullptr;
};

} // namespace streamproof

#endif
