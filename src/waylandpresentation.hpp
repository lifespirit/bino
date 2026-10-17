/*
 * Native Wayland presentation feedback for experimental shutter stereo.
 * Copyright (C) 2026
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <memory>
class QWindow;

// Observation only: never mistake a Qt swap callback for physical scanout.
// wl_surface retrieval relies on QtWayland >=6.9 winId() semantics.
class PresentationTracker
{
public:
    PresentationTracker();
    ~PresentationTracker();
    bool start(QWindow* window);
    void beforeSwap(int eye);
    bool enabled() const;
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
