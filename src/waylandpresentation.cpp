/*
 * Native Wayland presentation feedback for experimental shutter stereo.
 * Copyright (C) 2026
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "waylandpresentation.hpp"
#include <QGuiApplication>
#include <QWindow>
#include <QElapsedTimer>
#include <QtGlobal>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <cmath>
#include <unordered_set>

#if defined(WITH_WAYLAND_PRESENTATION) && QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
// QNativeInterface::QWaylandApplication is declared by QGuiApplication.
#include <wayland-client.h>
#include "presentation-time-client-protocol.h"

struct PresentationTracker::Impl
{
    struct Sample {
        Impl* owner;
        struct wp_presentation_feedback* feedback;
        int eye;
        QElapsedTimer requested;
        uint64_t swapTimestampNs = 0;
    };
    wl_display* display = nullptr;   // borrowed from QtWayland
    wl_surface* surface = nullptr;   // borrowed from QWindow
    wp_presentation* presentation = nullptr;
    std::unordered_set<Sample*> pending;
    int presentationClockId = -1;
    uint64_t lastSequence = 0;
    int lastEye = -1;
    bool haveSequence = false;
    uint64_t presented = 0, discarded = 0, skippedVblanks = 0, parityErrors = 0;
    uint64_t seqZero = 0, seqValid = 0, vsynced = 0, hwClock = 0, hwCompletion = 0, zeroCopy = 0;
    uint64_t timeMissedRefresh = 0, timeParityErrors = 0, timeUncertainGaps = 0;
    uint64_t timeGapCycles = 0;
    uint64_t swapToPresentSumNs = 0, swapToPresentMaxNs = 0, swapToPresentSamples = 0;
    uint64_t swapAfterPresent = 0;
    Sample* awaitingSwap = nullptr;
    uint64_t feedbackSamples = 0, feedbackLatencyNs = 0;
    uint64_t feedbackOverflow = 0;
    uint64_t lastPresentationNs = 0;
    uint64_t actualPresentationGapNs = 0, maxPresentationGapNs = 0;
    uint64_t actualGapSamples = 0, callbackAfterPresentNs = 0, callbackLagSamples = 0;
    uint32_t lastRefreshNs = 0;

    static void global(void* data, wl_registry* registry, uint32_t name,
                       const char* interface, uint32_t version)
    {
        auto* self = static_cast<Impl*>(data);
        if (std::strcmp(interface, wp_presentation_interface.name) == 0) {
            const uint32_t supported = version < 2u ? version : 2u;
            self->presentation = static_cast<wp_presentation*>(
                    wl_registry_bind(registry, name, &wp_presentation_interface, supported));
        }
    }
    static void globalRemoved(void*, wl_registry*, uint32_t) {}
    static void clockId(void* data, wp_presentation*, uint32_t id)
    {
        auto* self = static_cast<Impl*>(data);
        self->presentationClockId = static_cast<int>(id);
        std::fprintf(stderr, "bino: wayland-presentation clock_id=%u\n", id);
    }
    static void output(void*, struct wp_presentation_feedback*, wl_output*) {}
    static void shown(void* data, struct wp_presentation_feedback*,
                      uint32_t timeHi, uint32_t timeLo, uint32_t nsec,
                      uint32_t refresh, uint32_t seqHi, uint32_t seqLo, uint32_t flags)
    {
        auto* sample = static_cast<Sample*>(data);
        const uint64_t seq = (uint64_t(seqHi) << 32) | seqLo;
        const uint64_t whenNs = ((uint64_t(timeHi) << 32) | timeLo) * 1000000000ull + nsec;
        sample->owner->complete(sample, true, seq, refresh, whenNs, flags);
    }
    static void dropped(void* data, struct wp_presentation_feedback*)
    {
        auto* sample = static_cast<Sample*>(data);
        sample->owner->complete(sample, false, 0, 0, 0, 0);
    }
    void complete(Sample* sample, bool wasPresented, uint64_t sequence,
                  uint32_t refresh, uint64_t timestampNs, uint32_t flags)
    {
        if (wasPresented) {
            ++presented;
            if (sequence == 0)
                ++seqZero;
            else
                ++seqValid;
            if (flags & WP_PRESENTATION_FEEDBACK_KIND_VSYNC) ++vsynced;
            if (flags & WP_PRESENTATION_FEEDBACK_KIND_HW_CLOCK) ++hwClock;
            if (flags & WP_PRESENTATION_FEEDBACK_KIND_HW_COMPLETION) ++hwCompletion;
            if (flags & WP_PRESENTATION_FEEDBACK_KIND_ZERO_COPY) ++zeroCopy;
            if (refresh > 0)
                lastRefreshNs = refresh;
            if (lastPresentationNs && timestampNs > lastPresentationNs) {
                const uint64_t gap = timestampNs - lastPresentationNs;
                actualPresentationGapNs += gap;
                ++actualGapSamples;
                if (gap > maxPresentationGapNs)
                    maxPresentationGapNs = gap;
                // KWin may report seq=0 when the scanout MSC counter is
                // unavailable. Infer refresh cycles from timestamps, but
                // mark intervals that do not fit the known period as uncertain.
                if (lastRefreshNs > 0 && lastEye >= 0) {
                    const auto cycles = static_cast<uint64_t>(
                            std::llround(double(gap) / double(lastRefreshNs)));
                    const double difference = std::abs(
                            double(gap) - double(cycles) * double(lastRefreshNs));
                    if (cycles >= 1 && cycles < 100
                            && difference <= double(lastRefreshNs) * 0.25) {
                        timeGapCycles += cycles;
                        if (cycles > 1)
                            timeMissedRefresh += cycles - 1;
                        if ((sample->eye ^ lastEye) != int(cycles & 1u))
                            ++timeParityErrors;
                    } else {
                        ++timeUncertainGaps;
                    }
                }
            }
            if (timestampNs)
                lastPresentationNs = timestampNs;
            if (timestampNs && sample->swapTimestampNs > 0) {
                if (timestampNs >= sample->swapTimestampNs) {
                    const uint64_t gap = timestampNs - sample->swapTimestampNs;
                    if (gap < 250000000ULL) {
                        swapToPresentSumNs += gap;
                        ++swapToPresentSamples;
                        if (gap > swapToPresentMaxNs)
                            swapToPresentMaxNs = gap;
                    }
                } else if (sample->swapTimestampNs - timestampNs < 250000000ULL) {
                    // Qt frameSwapped can occur after the presented timestamp.
                    ++swapAfterPresent;
                }
            }
            if (timestampNs && presentationClockId >= 0) {
                timespec ts{};
                if (clock_gettime(clockid_t(presentationClockId), &ts) == 0) {
                    const uint64_t nowNs = uint64_t(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
                    if (nowNs >= timestampNs) {
                        callbackAfterPresentNs += nowNs - timestampNs;
                        ++callbackLagSamples;
                    }
                }
            }
            if (sequence != 0 && haveSequence && sequence > lastSequence) {
                const uint64_t delta = sequence - lastSequence;
                if (delta > 1)
                    skippedVblanks += delta - 1;
                // If a refresh was skipped, two consecutive *presented*
                // eyes should follow the parity of the actual refresh delta.
                if (lastEye >= 0 && (sample->eye ^ lastEye) != int(delta & 1u))
                    ++parityErrors;
            }
            if (sequence != 0) {
                lastSequence = sequence;
                haveSequence = true;
            } else {
                haveSequence = false;
            }
            lastEye = sample->eye;
        } else {
            ++discarded;
        }
        if (sample->requested.isValid()) {
            ++feedbackSamples;
            feedbackLatencyNs += uint64_t(sample->requested.nsecsElapsed());
        }
        if (awaitingSwap == sample)
            awaitingSwap = nullptr;
        wp_presentation_feedback_destroy(sample->feedback);
        pending.erase(sample);
        delete sample;
        if (presented + discarded >= 600) {
            std::fprintf(stderr,
                    "bino: wayland-presentation stats: presented=%llu discarded=%llu "
                    "skipped_vblanks=%llu eye_parity_errors=%llu "
                    "time_missed_refresh=%llu time_eye_parity_errors=%llu "
                    "time_uncertain=%llu time_cycles=%llu seq_zero=%llu seq_valid=%llu "
                    "vsynced=%llu hw_clock=%llu hw_completion=%llu zero_copy=%llu "
                    "pending=%zu feedback_overflow=%llu refresh=%.3fms "
                    "present_gap_avg=%.3fms present_gap_max=%.3fms "
                    "swap_to_present_avg=%.3fms swap_to_present_max=%.3fms "
                    "swap_after_present=%llu "
                    "callback_after_present_avg=%.3fms feedback_avg=%.3fms clock_id=%d\n",
                    (unsigned long long)presented,
                    (unsigned long long)discarded,
                    (unsigned long long)skippedVblanks,
                    (unsigned long long)parityErrors,
                    (unsigned long long)timeMissedRefresh,
                    (unsigned long long)timeParityErrors,
                    (unsigned long long)timeUncertainGaps,
                    (unsigned long long)timeGapCycles,
                    (unsigned long long)seqZero, (unsigned long long)seqValid,
                    (unsigned long long)vsynced, (unsigned long long)hwClock,
                    (unsigned long long)hwCompletion, (unsigned long long)zeroCopy,
                    pending.size(), (unsigned long long)feedbackOverflow,
                    double(lastRefreshNs) / 1e6,
                    actualGapSamples ? double(actualPresentationGapNs) / actualGapSamples / 1e6 : 0.,
                    double(maxPresentationGapNs) / 1e6,
                    swapToPresentSamples ? double(swapToPresentSumNs) / swapToPresentSamples / 1e6 : 0.,
                    double(swapToPresentMaxNs) / 1e6,
                    (unsigned long long)swapAfterPresent,
                    callbackLagSamples ? double(callbackAfterPresentNs) / callbackLagSamples / 1e6 : 0.,
                    feedbackSamples ? double(feedbackLatencyNs) / feedbackSamples / 1e6 : 0.,
                    presentationClockId);
            presented = discarded = skippedVblanks = parityErrors = 0;
            seqZero = seqValid = vsynced = hwClock = hwCompletion = zeroCopy = 0;
            timeMissedRefresh = timeParityErrors = timeUncertainGaps = timeGapCycles = 0;
            swapToPresentSumNs = swapToPresentMaxNs = swapToPresentSamples = 0;
            swapAfterPresent = 0;
            feedbackSamples = feedbackLatencyNs = 0;
            feedbackOverflow = 0;
            actualPresentationGapNs = maxPresentationGapNs = 0;
            actualGapSamples = callbackAfterPresentNs = callbackLagSamples = 0;
        }
    }
    bool initialize(QWindow* window)
    {
        if (QGuiApplication::platformName() != QLatin1String("wayland"))
            return false;
        auto* qt = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
        if (!qt || !(display = qt->display()))
            return false;
        surface = reinterpret_cast<wl_surface*>(window->winId());
        if (!surface)
            return false;
        wl_registry* registry = wl_display_get_registry(display);
        if (!registry)
            return false;
        static const wl_registry_listener listener{ &Impl::global, &Impl::globalRemoved };
        wl_registry_add_listener(registry, &listener, this);
        const int rc = wl_display_roundtrip(display);
        wl_registry_destroy(registry);
        if (rc < 0 || !presentation)
            return false;
        static const wp_presentation_listener presentationListener{ &Impl::clockId };
        wp_presentation_add_listener(presentation, &presentationListener, this);
        std::fprintf(stderr, "bino: wayland-presentation enabled for native wl_surface=%p\n",
                     static_cast<void*>(surface));
        return true;
    }
    void record(int eye)
    {
        if (!presentation || !surface || eye < 0 || eye > 1)
            return;
        // Bound memory if compositor feedback stalls or disappears.
        if (pending.size() >= 16) {
            ++feedbackOverflow;
            if (feedbackOverflow == 1 || feedbackOverflow % 600 == 0)
                std::fprintf(stderr,
                        "bino: wayland-presentation: no feedback, pending=%zu; "
                        "skipping request (overflow=%llu)\n",
                        pending.size(), (unsigned long long)feedbackOverflow);
            return;
        }
        auto* feedback = wp_presentation_feedback(presentation, surface);
        if (!feedback)
            return;
        auto* sample = new Sample{this, feedback, eye, QElapsedTimer{}, 0};
        sample->requested.start();
        pending.insert(sample);
        awaitingSwap = sample;
        static const wp_presentation_feedback_listener listener{
            &Impl::output, &Impl::shown, &Impl::dropped
        };
        wp_presentation_feedback_add_listener(feedback, &listener, sample);
    }
    void noteSwapped()
    {
        if (!awaitingSwap)
            return;
        if (presentationClockId >= 0) {
            timespec ts{};
            if (clock_gettime(clockid_t(presentationClockId), &ts) == 0) {
                awaitingSwap->swapTimestampNs =
                    uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
            }
        }
        awaitingSwap = nullptr;
    }
    ~Impl()
    {
        for (Sample* sample : pending) {
            wp_presentation_feedback_destroy(sample->feedback);
            delete sample;
        }
        if (presentation)
            wp_presentation_destroy(presentation);
    }
};
#else
struct PresentationTracker::Impl {};
#endif

PresentationTracker::PresentationTracker() = default;
PresentationTracker::~PresentationTracker() = default;

bool PresentationTracker::start(QWindow* window)
{
#if defined(WITH_WAYLAND_PRESENTATION) && QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    auto impl = std::make_unique<Impl>();
    if (!impl->initialize(window)) {
        std::fprintf(stderr,
                "bino: wayland-presentation unavailable; Qt swap timestamps are not scanout times\n");
        return false;
    }
    _impl = std::move(impl);
    return true;
#else
    Q_UNUSED(window);
    std::fprintf(stderr, "bino: wayland-presentation unavailable (needs "
            "QtWayland >=6.9 and optional wayland-protocols build dependencies)\n");
    return false;
#endif
}
void PresentationTracker::beforeSwap(int eye)
{
#if defined(WITH_WAYLAND_PRESENTATION) && QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    if (_impl)
        _impl->record(eye);
#else
    Q_UNUSED(eye);
#endif
}
void PresentationTracker::afterSwap()
{
#if defined(WITH_WAYLAND_PRESENTATION) && QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    if (_impl)
        _impl->noteSwapped();
#endif
}
bool PresentationTracker::enabled() const
{
    return bool(_impl);
}
