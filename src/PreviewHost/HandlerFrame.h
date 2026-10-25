#pragma once

#include <windows.h>
#include <shobjidl.h>

#include <atomic>
#include <cstdint>

namespace fastlook::host
{
    // Receives a key a handler window wants the Viewer to act on (vk + kMod* bits).
    using KeySink = void (*)(uint32_t vk, uint32_t mods);

    // If `msg` is a WM_KEYDOWN/WM_SYSKEYDOWN of a forwarded key (IsForwardedHostKey, with the modifiers currently held), hands it
    // to `sink` and returns true.
    bool ForwardKeyMessage(MSG const& msg, KeySink sink);

    // The host's IPreviewHandlerFrame: the handler asks it for the accelerators it must report (Esc/Space/arrows/...) and
    // reports back keys it did not consume, which travel to the Viewer as `key` lines.
    class HandlerFrame final : public IPreviewHandlerFrame
    {
    public:
        explicit HandlerFrame(KeySink sink) : m_sink(sink) {}
        ~HandlerFrame();

        // IUnknown
        IFACEMETHODIMP QueryInterface(REFIID iid, void** object) override;
        IFACEMETHODIMP_(ULONG) AddRef() override { return ++m_refs; }
        IFACEMETHODIMP_(ULONG) Release() override;
        // IPreviewHandlerFrame
        IFACEMETHODIMP GetWindowContext(PREVIEWHANDLERFRAMEINFO* info) override;
        IFACEMETHODIMP TranslateAccelerator(MSG* msg) override;

    private:
        KeySink m_sink;
        HACCEL m_accel = nullptr;
        UINT m_accelCount = 0;
        std::atomic<ULONG> m_refs{1};
    };
}
