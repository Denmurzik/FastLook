#include "pch.h"
#include "WindowAnimator.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>

#include <dwmapi.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>

#include "Diagnostics/Log.h"
#include "Diagnostics/PerfLog.h"
#include "Util/Async.h"

#pragma comment(lib, "dwmapi.lib")

namespace fastlook::viewer::anim
{
    namespace
    {
        using namespace std::chrono_literals;
        namespace comp = winrt::Microsoft::UI::Composition;
        using winrt::Microsoft::UI::Xaml::Media::CompositionTarget;
        using winrt::Microsoft::UI::Xaml::Window;
        using Clock = std::chrono::steady_clock;

        constexpr auto kInDuration = 180ms;
        constexpr auto kOutDuration = 120ms;
        constexpr auto kResizeDuration = 150ms;
        constexpr auto kResizeTick = 16ms;
        constexpr auto kFirstFrameTimeout = 250ms;
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

        double EaseOut(double t)
        {
            t = std::clamp(t, 0.0, 1.0);
            return 1.0 - std::pow(1.0 - t, 3.0);
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
        // there once the animation completes.
        void Animate(comp::Visual const& visual, Collapsed const& pose, float fromScale, float fromOpacity, float toScale,
                     float toOpacity, std::chrono::milliseconds duration)
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

            visual.StartAnimation(L"Scale", scaleAnim);
            visual.StartAnimation(L"Opacity", opacityAnim);
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

        // The In animation: waits for the first rendered frame, then runs; a failsafe timer snaps to the final pose when
        // no frame arrives (occluded / minimised window).
        struct InState
        {
            bool pending = false;
            winrt::event_token token{};
            uint64_t generation = 0;
            winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer failsafe{nullptr};
            winrt::event_token failsafeTick{};
            // Set once the animation started: lets Out begin from the pose the In animation has reached.
            bool running = false;
            Clock::time_point animStart;
            float fromScale = 1.0f;
        };

        // Intentionally leaked: they hold WinRT objects that must not be destroyed by CRT static destruction, which runs
        // after the apartment teardown (same pattern as Str.cpp).
        ResizeState& Resizing()
        {
            static auto* state = new ResizeState;
            return *state;
        }

        InState& Showing()
        {
            static auto* state = new InState;
            return *state;
        }

        void StopResize()
        {
            auto& r = Resizing();
            if (r.timer)
            {
                r.timer.Stop();
                r.timer.Tick(r.tick);
                r.timer = nullptr;
            }
            r.window = nullptr;
        }

        void StopPendingIn()
        {
            auto& in = Showing();
            if (in.pending)
            {
                in.pending = false;
                CompositionTarget::Rendering(in.token);
            }
            if (in.failsafe)
            {
                in.failsafe.Stop();
                in.failsafe.Tick(in.failsafeTick);
                in.failsafe = nullptr;
            }
        }

        // The composition batch's Completed event reaches the UI thread ~165 ms after the animation really ends (a 20 ms
        // probe animation reports at ~190 ms), so the end mark waits out the duration instead; the start latency mark
        // (anim_start) is what shows whether the animation actually began.
        winrt::fire_and_forget MarkAnimInAsync(Clock::time_point start)
        {
            try
            {
                co_await winrt::resume_after(kInDuration);
                core::perf::Mark("anim_in", "ms=" + Ms(MillisSince(start)));
            }
            catch (...)
            {
                FL_LOG_INFO(L"anim::MarkAnimInAsync failed");
            }
        }

        int Lerp(int a, int b, double t)
        {
            return static_cast<int>(std::lround(a + (b - a) * t));
        }

        void OnResizeTick()
        {
            auto& r = Resizing();
            if (!r.window)
            {
                return;
            }
            auto const window = r.window;
            auto const a = r.from;
            auto const b = r.to;
            double const ms = MillisSince(r.start);
            double const t = std::min(1.0, ms / std::chrono::duration<double, std::milli>(kResizeDuration).count());
            if (t >= 1.0)
            {
                StopResize();
                Place(window, b);
                core::perf::Mark("resize", "from=" + std::to_string(a.w) + "x" + std::to_string(a.h) + " to=" + std::to_string(b.w) +
                                               "x" + std::to_string(b.h) + " ms=" + Ms(ms));
                return;
            }
            double const eased = EaseOut(t);
            Place(window, {Lerp(a.x, b.x, eased), Lerp(a.y, b.y, eased), Lerp(a.w, b.w, eased), Lerp(a.h, b.h, eased)});
        }

        void OnFirstFrame(uint64_t generation, Clock::time_point shown, comp::Visual const& visual, Collapsed const& pose)
        {
            auto& state = Showing();
            if (generation != state.generation || !state.pending)
            {
                return;
            }
            StopPendingIn();
            auto const start = Clock::now();
            core::perf::Mark("anim_start", "ms=" + Ms(std::chrono::duration<double, std::milli>(start - shown).count()));
            Animate(visual, pose, pose.scale, 0.0f, 1.0f, 1.0f, kInDuration);
            state.running = true;
            state.animStart = start;
            state.fromScale = pose.scale;
            MarkAnimInAsync(start);
        }

        void OnFirstFrameTimeout(uint64_t generation, comp::Visual const& visual, Collapsed const& pose)
        {
            auto& state = Showing();
            if (generation != state.generation || !state.pending)
            {
                return;
            }
            StopPendingIn();
            SetPose(visual, pose, 1.0f, 1.0f);
            core::perf::Mark("anim_failsafe");
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
        StopPendingIn();
        Showing().running = false;
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
        auto& in = Showing();
        uint64_t const generation = ++in.generation;
        auto const shown = Clock::now();
        try
        {
            in.token = CompositionTarget::Rendering([visual, pose, generation, shown](auto const&, auto const&) {
                try
                {
                    OnFirstFrame(generation, shown, visual, pose);
                }
                catch (...)
                {
                    FL_LOG_INFO(L"anim::In first-frame callback failed");
                }
            });
            in.pending = true;

            // No frame ever arrives for an occluded / minimised window: do not leave the content invisible.
            in.failsafe = window.DispatcherQueue().CreateTimer();
            in.failsafe.Interval(kFirstFrameTimeout);
            in.failsafe.IsRepeating(false);
            in.failsafeTick = in.failsafe.Tick([visual, pose, generation](auto const&, auto const&) {
                try
                {
                    OnFirstFrameTimeout(generation, visual, pose);
                }
                catch (...)
                {
                    FL_LOG_INFO(L"anim::In failsafe callback failed");
                }
            });
            in.failsafe.Start();
        }
        catch (...)
        {
            FL_LOG_INFO(L"anim::In could not arm the animation, showing the final pose");
            StopPendingIn();
            SetPose(visual, pose, 1.0f, 1.0f);
        }
    }

    winrt::Windows::Foundation::IAsyncAction Out(Window const& window, std::optional<core::RectI> iconPx)
    {
        StopResize();
        auto& in = Showing();
        bool const wasPending = in.pending; // still collapsed and invisible: nothing to animate out
        // Where the In animation has got to, so Out continues from there instead of popping to the final pose.
        float fromScale = 1.0f;
        float fromOpacity = 1.0f;
        if (in.running)
        {
            double const t = EaseOut(MillisSince(in.animStart) / std::chrono::duration<double, std::milli>(kInDuration).count());
            fromScale = static_cast<float>(in.fromScale + (1.0 - in.fromScale) * t);
            fromOpacity = static_cast<float>(t);
        }
        in.running = false;
        StopPendingIn();
        uint64_t const generation = in.generation;

        auto const visual = RootVisual(window);
        if (!visual || !Enabled() || wasPending)
        {
            co_return;
        }
        auto const dq = window.DispatcherQueue();
        auto const pose = CollapsedPose(window, iconPx, BoundsOf(window));
        visual.StopAnimation(L"Opacity");
        visual.StopAnimation(L"Scale");
        Animate(visual, pose, fromScale, fromOpacity, pose.scale, 0.0f, kOutDuration);

        co_await winrt::resume_after(kOutDuration + 20ms);
        // False only when the queue is shutting down; the caller hides the window either way.
        if (co_await ResumeOn(dq) && Showing().generation == generation) // not shown again meanwhile
        {
            // Done: detach the animations before the window hides (the base values already hold the end pose). Animations still
            // attached to the root visual of a hidden window leak composition resources; the caller restores the pose after Hide.
            visual.StopAnimation(L"Opacity");
            visual.StopAnimation(L"Scale");
        }
    }

    void Resize(Window const& window, core::RectI, core::RectI toPx)
    {
        StopResize();
        core::RectI const fromPx = BoundsOf(window); // the actual bounds: a previous resize may have been cut short
        bool const large = std::abs(toPx.w - fromPx.w) > fromPx.w * 0.1 || std::abs(toPx.h - fromPx.h) > fromPx.h * 0.1;
        if (!large || !Enabled())
        {
            Place(window, toPx);
            return;
        }

        auto& r = Resizing();
        r.window = window;
        r.from = fromPx;
        r.to = toPx;
        r.start = Clock::now();
        r.timer = window.DispatcherQueue().CreateTimer();
        r.timer.Interval(kResizeTick);
        r.tick = r.timer.Tick([](auto const&, auto const&) {
            try
            {
                OnResizeTick();
            }
            catch (...)
            {
                StopResize();
                FL_LOG_INFO(L"anim::Resize tick failed");
            }
        });
        r.timer.Start();
    }
}
