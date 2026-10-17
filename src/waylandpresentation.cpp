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
#include <unordered_set>

#if defined(WITH_WAYLAND_PRESENTATION) && QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
#include <QNativeInterface>
#include <wayland-client.h>
#include "presentation-time-client-protocol.h"

struct PresentationTracker::Impl
{
    struct Sample {
        Impl* owner;
        wp_presentation_feedback* feedback;
        int eye;
        QElapsedTimer requested;
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
    uint64_t feedbackSamples = 0, feedbackLatencyNs = 0;
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
    static void output(void*, wp_presentation_feedback*, wl_output*) {}
    static void shown(void* data, wp_presentation_feedback*,
                      uint32_t, uint32_t, uint32_t,
                      uint32_t refresh, uint32_t seqHi, uint32_t seqLo, uint32_t)
    {
        auto* sample = static_cast<Sample*>(data);
        const uint64_t seq = (uint64_t(seqHi) << 32) | seqLo;
        sample->owner->complete(sample, true, seq, refresh);
    }
    static void dropped(void* data, wp_presentation_feedback*)
    {
        auto* sample = static_cast<Sample*>(data);
        sample->owner->complete(sample, false, 0, 0);
    }
    void complete(Sample* sample, bool wasPresented, uint64_t sequence, uint32_t refresh)
    {
        if (wasPresented) {
            ++presented;
            lastRefreshNs = refresh;
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
                lastEye = sample->eye;
                haveSequence = true;
            }
        } else {
            ++discarded;
        }
        if (sample->requested.isValid()) {
            ++feedbackSamples;
            feedbackLatencyNs += uint64_t(sample->requested.nsecsElapsed());
        }
        wp_presentation_feedback_destroy(sample->feedback);
        pending.erase(sample);
        delete sample;
        if (presented + discarded >= 600) {
            std::fprintf(stderr,
                    "bino: wayland-presentation stats: presented=%llu discarded=%llu "
                    "skipped_vblanks=%llu eye_parity_errors=%llu "
                    "pending=%zu refresh=%.3fms feedback_avg=%.3fms "
                    "clock_id=%d\n",
                    (unsigned long long)presented,
                    (unsigned long long)discarded,
                    (unsigned long long)skippedVblanks,
                    (unsigned long long)parityErrors,
                    pending.size(), double(lastRefreshNs) / 1e6,
                    feedbackSamples ? double(feedbackLatencyNs) / feedbackSamples / 1e6 : 0.,
                    presentationClockId);
            presented = discarded = skippedVblanks = parityErrors = 0;
            feedbackSamples = feedbackLatencyNs = 0;
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
        auto* feedback = wp_presentation_feedback(presentation, surface);
        if (!feedback)
            return;
        auto* sample = new Sample{this, feedback, eye, QElapsedTimer{}};
        sample->requested.start();
        pending.insert(sample);
        static const wp_presentation_feedback_listener listener{
            &Impl::output, &Impl::shown, &Impl::dropped
        };
        wp_presentation_feedback_add_listener(feedback, &listener, sample);
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
bool PresentationTracker::enabled() const
{
    return bool(_impl);
}
