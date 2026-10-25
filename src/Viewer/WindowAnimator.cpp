#include "pch.h"
#include "WindowAnimator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <string>

#include <dwmapi.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>

#include "Diagnostics/PerfLog.h"
#include "Util/Async.h"

#pragma comment(lib, "dwmapi.lib")

namespace fastlook::viewer::anim
{
    namespace
    {
        using namespace std::chrono_literals;
        namespace comp = winrt::Microsoft::UI::Composition;
        using winrt::Microsoft::UI::Xaml::Window;
        using Clock = std::chrono::steady_clock;

        constexpr auto kInDuration = 180ms;
        constexpr auto kOutDuration = 120ms;
        constexpr auto kResizeDuration = 150ms;
        constexpr auto kResizeTick = 16ms;
        constexpr float kFallbackScale = 0.95f;
        constexpr float kMinScale = 0.05f;

        double MillisSince(Clock::time_point start)
        {
            return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        }

        std::string Ms(double ms)
        {
            return std::to_string(static_cast<int>(std::lround(ms)));
        }

        HWND HwndOf(Window const& window)
        {
            HWND hwnd = nullptr;
            if (auto native = window.try_as<::IWindowNative>())
            {
                native->get_WindowHandle(&hwnd);
            }
            return hwnd;
        }

        core::RectI BoundsOf(Window const& window)
        {
            auto const position = window.AppWindow().Position();
            auto const size = window.AppWindow().Size();
            return {position.X, position.Y, size.Width, size.Height};
        }

        void Place(Window const& window, core::RectI r)
        {
            window.AppWindow().MoveAndResize({r.x, r.y, r.w, r.h});
        }

        comp::Visual RootVisual(Window const& window)
        {
            auto const content = window.Content();
            return content ? winrt::Microsoft::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(content) : nullptr;
        }

        float RasterScale(Window const& window)
        {
            auto const content = window.Content();
            auto const root = content ? content.XamlRoot() : nullptr;
            return root ? static_cast<float>(root.RasterizationScale()) : 1.0f;
        }

        // The "collapsed" pose of the root visual: scale about `center` (DIPs, window-relative) plus opacity 0.
        struct Collapsed
        {
            float scale = kFallbackScale;
            winrt::Windows::Foundation::Numerics::float3 center{};
        };

        Collapsed CollapsedPose(Window const& window, std::optional<core::RectI> iconPx, core::RectI windowPx)
        {
            float const dpi = RasterScale(window);
            Collapsed pose;
            pose.center = {windowPx.w / dpi / 2.0f, windowPx.h / dpi / 2.0f, 0.0f};
            if (iconPx && iconPx->w > 0 && windowPx.w > 0)
            {
                pose.scale = std::clamp(static_cast<float>(iconPx->w) / static_cast<float>(windowPx.w), kMinScale, 1.0f);
                float const cx = (iconPx->x + iconPx->w / 2.0f - windowPx.x) / dpi;
                float const cy = (iconPx->y + iconPx->h / 2.0f - windowPx.y) / dpi;
                pose.center = {std::clamp(cx, 0.0f, windowPx.w / dpi), std::clamp(cy, 0.0f, windowPx.h / dpi), 0.0f};
            }
            return pose;
        }

        void SetPose(comp::Visual const& visual, Collapsed const& pose, float scale, float opacity)
        {
            visual.StopAnimation(L"Opacity");
            visual.StopAnimation(L"Scale");
            visual.CenterPoint(pose.center);
            visual.Scale({scale, scale, 1.0f});
            visual.Opacity(opacity);
        }

        // Animates scale and opacity between two poses. The base values are set to the end pose first so the visual rests
        // there once the animation completes. `done` runs when the batch completes.
        void Animate(comp::Visual const& visual, Collapsed const& pose, float fromScale, float fromOpacity, float toScale,
                     float toOpacity, std::chrono::milliseconds duration, std::function<void()> done)
        {
            auto const compositor = visual.Compositor();
            visual.CenterPoint(pose.center);
            visual.Scale({toScale, toScale, 1.0f});
            visual.Opacity(toOpacity);

            auto const easing = compositor.CreateCubicBezierEasingFunction({0.215f, 0.61f}, {0.355f, 1.0f}); // ease-out cubic

            auto const scaleAnim = compositor.CreateVector3KeyFrameAnimation();
            scaleAnim.InsertKeyFrame(0.0f, {fromScale, fromScale, 1.0f});
            scaleAnim.InsertKeyFrame(1.0f, {toScale, toScale, 1.0f}, easing);
            scaleAnim.Duration(duration);

            auto const opacityAnim = compositor.CreateScalarKeyFrameAnimation();
            opacityAnim.InsertKeyFrame(0.0f, fromOpacity);
            opacityAnim.InsertKeyFrame(1.0f, toOpacity, easing);
            opacityAnim.Duration(duration);

            auto const batch = compositor.CreateScopedBatch(comp::CompositionBatchTypes::Animation);
            visual.StartAnimation(L"Scale", scaleAnim);
            visual.StartAnimation(L"Opacity", opacityAnim);
            batch.End();
            if (done)
            {
                batch.Completed([done = std::move(done)](auto const&, auto const&) { done(); });
            }
        }

        // Stepped window resize; only one runs at a time.
        struct ResizeState
        {
            winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer timer{nullptr};
            winrt::event_token tick{};
            Window window{nullptr};
            core::RectI from, to;
            Clock::time_point start;
        };
        ResizeState g_resize;

        void StopResize()
        {
            if (g_resize.timer)
            {
                g_resize.timer.Stop();
                g_resize.timer.Tick(g_resize.tick);
                g_resize.timer = nullptr;
            }
            g_resize.window = nullptr;
        }

        // The In animation waiting for its first rendered frame.
        struct PendingIn
        {
            bool pending = false;
            winrt::event_token token{};
            uint64_t generation = 0;
        };
        PendingIn g_in;

        void StopPendingIn()
        {
            if (g_in.pending)
            {
                g_in.pending = false;
                winrt::Microsoft::UI::Xaml::Media::CompositionTarget::Rendering(g_in.token);
            }
        }

        // The composition batch's Completed event reaches the UI thread ~165 ms after the animation really ends (a 20 ms
        // probe animation reports at ~190 ms), so the elapsed time is taken after waiting out the duration instead.
        winrt::fire_and_forget MarkAnimInAsync(Clock::time_point start)
        {
            try
            {
                co_await winrt::resume_after(kInDuration);
                core::perf::Mark("anim_in", "ms=" + Ms(MillisSince(start)));
            }
            catch (...)
            {
            }
        }

        int Lerp(int a, int b, double t)
        {
            return static_cast<int>(std::lround(a + (b - a) * t));
        }

        void OnResizeTick()
        {
            if (!g_resize.window)
            {
                return;
            }
            auto const window = g_resize.window;
            auto const a = g_resize.from;
            auto const b = g_resize.to;
            double const ms = MillisSince(g_resize.start);
            double const t = std::min(1.0, ms / std::chrono::duration<double, std::milli>(kResizeDuration).count());
            if (t >= 1.0)
            {
                StopResize();
                Place(window, b);
                core::perf::Mark("resize", "from=" + std::to_string(a.w) + "x" + std::to_string(a.h) + " to=" + std::to_string(b.w) +
                                               "x" + std::to_string(b.h) + " ms=" + Ms(ms));
                return;
            }
            double const eased = 1.0 - std::pow(1.0 - t, 3.0);
            Place(window, {Lerp(a.x, b.x, eased), Lerp(a.y, b.y, eased), Lerp(a.w, b.w, eased), Lerp(a.h, b.h, eased)});
        }
    }

    bool Enabled()
    {
        BOOL on = TRUE;
        return !SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0) || on != FALSE;
    }

    void In(Window const& window, std::optional<core::RectI> iconPx, core::RectI finalPx)
    {
        StopResize();
        if (HWND const hwnd = HwndOf(window))
        {
            BOOL const disable = TRUE;
            DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &disable, sizeof(disable));
        }

        auto const visual = RootVisual(window);
        bool const animate = visual && Enabled();
        Collapsed pose;
        if (animate)
        {
            pose = CollapsedPose(window, iconPx, finalPx);
            SetPose(visual, pose, pose.scale, 0.0f); // before the window shows: no flash of the final pose
        }
        else if (visual)
        {
            SetPose(visual, pose, 1.0f, 1.0f);
        }

        // The window is visible at its final bounds from the first frame; only the content animates.
        Place(window, finalPx);
        window.AppWindow().Show(true);
        if (!animate)
        {
            return;
        }

        // Start on the first frame the window renders: an animation started before that waits for the window to come up
        // and runs late (measured ~150 ms on a cold show).
        using winrt::Microsoft::UI::Xaml::Media::CompositionTarget;
        StopPendingIn();
        g_in.generation++;
        g_in.token = CompositionTarget::Rendering(
            [visual, pose, generation = g_in.generation](auto const&, auto const&) {
                if (generation != g_in.generation)
                {
                    return;
                }
                StopPendingIn();
                auto const start = Clock::now();
                Animate(visual, pose, pose.scale, 0.0f, 1.0f, 1.0f, kInDuration, nullptr);
                MarkAnimInAsync(start);
            });
        g_in.pending = true;
    }

    winrt::Windows::Foundation::IAsyncAction Out(Window const& window, std::optional<core::RectI> iconPx)
    {
        StopResize();
        StopPendingIn();
        auto const visual = RootVisual(window);
        if (!visual || !Enabled())
        {
            co_return;
        }
        auto const dq = window.DispatcherQueue();
        auto const pose = CollapsedPose(window, iconPx, BoundsOf(window));
        Animate(visual, pose, 1.0f, 1.0f, pose.scale, 0.0f, kOutDuration, nullptr);

        co_await winrt::resume_after(kOutDuration + 20ms);
        (void)co_await ResumeOn(dq); // false only when the queue is shutting down; the caller hides the window either way
    }

    void Resize(Window const& window, core::RectI fromPx, core::RectI toPx)
    {
        StopResize();
        bool const large = std::abs(toPx.w - fromPx.w) > fromPx.w * 0.1 || std::abs(toPx.h - fromPx.h) > fromPx.h * 0.1;
        if (!large || !Enabled())
        {
            Place(window, toPx);
            return;
        }

        g_resize.window = window;
        g_resize.from = fromPx;
        g_resize.to = toPx;
        g_resize.start = Clock::now();
        g_resize.timer = window.DispatcherQueue().CreateTimer();
        g_resize.timer.Interval(kResizeTick);
        g_resize.tick = g_resize.timer.Tick([](auto const&, auto const&) { OnResizeTick(); });
        g_resize.timer.Start();
    }
}
