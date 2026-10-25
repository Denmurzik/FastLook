#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "Geometry.h"
#include "Imaging/DecodeMath.h"

namespace fastlook::core
{
    enum class Dispose
    {
        None,
        Background,
        Previous
    };

    enum class Blend
    {
        Source,
        Over
    };

    struct FrameInfo
    {
        RectI rect;
        uint32_t delayMs = 100;
        Dispose dispose = Dispose::None;
        Blend blend = Blend::Over;
    };

    // Composites GIF/APNG sub-frames onto a canvas of premultiplied BGRA pixels (0xAARRGGBB). The disposal of the previous
    // frame is applied before the next one is drawn, so Canvas() always shows the frame as it is displayed.
    class FrameCompositor
    {
    public:
        void Begin(SizeU canvas);
        // Back to a blank canvas of the same size (loop wrap) without reallocating it.
        void Restart();
        // `framePBGRA` holds rect.w * rect.h pixels (stride = rect.w); the rect is clipped to the canvas.
        void Apply(FrameInfo const& frame, std::span<const uint32_t> framePBGRA);
        std::span<const uint32_t> Canvas() const { return m_canvas; }

    private:
        void DisposePrevious();

        SizeU m_size{};
        std::vector<uint32_t> m_canvas;
        std::vector<uint32_t> m_saved; // canvas before the previous frame was drawn, kept only for Dispose::Previous
        FrameInfo m_previous{};
        bool m_hasPrevious = false;
    };
}
