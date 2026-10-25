#include "Imaging/FrameCompositor.h"

#include <algorithm>

namespace fastlook::core
{
    namespace
    {
        // Clips `rect` to the canvas; false when nothing is left.
        bool Clip(RectI const& rect, SizeU canvas, RectI& out)
        {
            int64_t const x0 = std::max<int64_t>(rect.x, 0);
            int64_t const y0 = std::max<int64_t>(rect.y, 0);
            int64_t const x1 = std::min<int64_t>(static_cast<int64_t>(rect.x) + rect.w, canvas.w);
            int64_t const y1 = std::min<int64_t>(static_cast<int64_t>(rect.y) + rect.h, canvas.h);
            if (rect.w <= 0 || rect.h <= 0 || x1 <= x0 || y1 <= y0)
            {
                return false;
            }
            out = {static_cast<int32_t>(x0), static_cast<int32_t>(y0), static_cast<int32_t>(x1 - x0), static_cast<int32_t>(y1 - y0)};
            return true;
        }

        // Premultiplied source over destination.
        uint32_t Over(uint32_t src, uint32_t dst)
        {
            uint32_t const inverse = 255 - (src >> 24);
            if (inverse == 255)
            {
                return dst; // transparent source
            }
            uint32_t result = 0;
            for (int shift = 0; shift < 32; shift += 8)
            {
                uint32_t const s = (src >> shift) & 0xFF;
                uint32_t const d = (dst >> shift) & 0xFF;
                result |= std::min<uint32_t>(s + (d * inverse + 127) / 255, 255) << shift;
            }
            return result;
        }
    }

    void FrameCompositor::Begin(SizeU canvas)
    {
        m_size = canvas;
        m_canvas.assign(static_cast<size_t>(canvas.w) * canvas.h, 0);
        std::vector<uint32_t>().swap(m_saved); // only needed between a Dispose::Previous frame and the next one
        m_hasPrevious = false;
    }

    void FrameCompositor::Restart()
    {
        std::fill(m_canvas.begin(), m_canvas.end(), 0u);
        std::vector<uint32_t>().swap(m_saved);
        m_hasPrevious = false;
    }

    void FrameCompositor::DisposePrevious()
    {
        if (!m_hasPrevious)
        {
            return;
        }
        if (m_previous.dispose == Dispose::Background)
        {
            RectI clipped;
            if (Clip(m_previous.rect, m_size, clipped))
            {
                for (int32_t y = 0; y < clipped.h; ++y)
                {
                    auto const row = m_canvas.begin() + static_cast<size_t>(clipped.y + y) * m_size.w + clipped.x;
                    std::fill(row, row + clipped.w, 0u);
                }
            }
        }
        else if (m_previous.dispose == Dispose::Previous && m_saved.size() == m_canvas.size())
        {
            m_canvas = m_saved;
        }
        std::vector<uint32_t>().swap(m_saved); // only needed between a Dispose::Previous frame and the next one
    }

    void FrameCompositor::Apply(FrameInfo const& frame, std::span<const uint32_t> framePBGRA)
    {
        if (m_canvas.empty())
        {
            return;
        }
        DisposePrevious();
        if (frame.dispose == Dispose::Previous)
        {
            m_saved = m_canvas;
        }
        m_previous = frame;
        m_hasPrevious = true;

        RectI clipped;
        if (!Clip(frame.rect, m_size, clipped) || framePBGRA.size() < static_cast<size_t>(frame.rect.w) * frame.rect.h)
        {
            return;
        }
        for (int32_t y = 0; y < clipped.h; ++y)
        {
            size_t const srcRow = static_cast<size_t>(clipped.y - frame.rect.y + y) * frame.rect.w + (clipped.x - frame.rect.x);
            size_t const dstRow = static_cast<size_t>(clipped.y + y) * m_size.w + clipped.x;
            for (int32_t x = 0; x < clipped.w; ++x)
            {
                uint32_t const src = framePBGRA[srcRow + x];
                m_canvas[dstRow + x] = frame.blend == Blend::Source ? src : Over(src, m_canvas[dstRow + x]);
            }
        }
    }
}
