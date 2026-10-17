/*
 * Experimental NVIDIA 3D Vision emitter bridge for Bino.
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "nvstusbcontroller.hpp"

#include <cstdio>
#include <QByteArray>

#ifdef WITH_NVSTUSB
extern "C" {
#include <nvstusb.h>
}
#endif

NvstusbController::NvstusbController(QObject* parent)
    : QObject(parent)
{
    _phaseTimer.setSingleShot(true);
    _phaseTimer.setTimerType(Qt::PreciseTimer);
    connect(&_phaseTimer, &QTimer::timeout, this, [this]() { sendPendingEye(); });
}

NvstusbController::~NvstusbController()
{
    _phaseTimer.stop();
#ifdef WITH_NVSTUSB
    if (_ctx)
        nvstusb_deinit(_ctx);
#endif
}

bool NvstusbController::start(const QString& firmware, float rate, int phaseMs, bool invert)
{
#ifdef WITH_NVSTUSB
    if (_ctx)
        return false;
    if (rate <= 60.0f || phaseMs < 0 || phaseMs > 7)
        return false;

    const QByteArray file = firmware.toLocal8Bit();
    _ctx = nvstusb_init(file.isEmpty() ? nullptr : file.constData());
    if (!_ctx) {
        std::fprintf(stderr, "bino: nvstusb: unable to initialize USB emitter\n");
        return false;
    }

    nvstusb_set_rate(_ctx, rate);
    const int error = nvstusb_last_error(_ctx);
    if (error != 0) {
        std::fprintf(stderr, "bino: nvstusb: failed to program refresh rate (USB %d)\n", error);
        nvstusb_deinit(_ctx);
        _ctx = nullptr;
        return false;
    }
    if (invert)
        nvstusb_invert_eyes(_ctx);

    _phaseMs = phaseMs;
    std::fprintf(stderr,
            "bino: nvstusb: initialized at %.2f Hz, phase delay %d ms, invert %s\n"
            "bino: nvstusb: experimental Qt frameSwapped timing, NOT physical display sync\n",
            rate, phaseMs, invert ? "yes" : "no");
    return true;
#else
    Q_UNUSED(firmware);
    Q_UNUSED(rate);
    Q_UNUSED(phaseMs);
    Q_UNUSED(invert);
    std::fprintf(stderr, "bino: nvstusb: this binary was built without ENABLE_NVSTUSB\n");
    return false;
#endif
}

bool NvstusbController::enabled() const
{
    return _ctx != nullptr;
}

void NvstusbController::onFrameSwapped(int eye)
{
    if (!_ctx || (eye != 0 && eye != 1))
        return;

    _pendingEye = eye;
    if (_phaseMs == 0) {
        _phaseTimer.stop();
        sendPendingEye();
    } else {
        // Replace a queued eye with the latest one. Never emit stale shutters.
        _phaseTimer.start(_phaseMs);
    }
}

void NvstusbController::sendPendingEye()
{
#ifdef WITH_NVSTUSB
    if (!_ctx || _pendingEye < 0)
        return;

    const int eye = _pendingEye;
    _pendingEye = -1;

    // This API sends the eye command directly and does not access GLX.
    const int error = nvstusb_send_eye(_ctx, eye == 0 ? nvstusb_left : nvstusb_right);
    if (error != 0) {
        std::fprintf(stderr,
                "bino: nvstusb: shutter USB transfer failed (%d); disabling emitter\n",
                error);
        _phaseTimer.stop();
        nvstusb_deinit(_ctx);
        _ctx = nullptr;
    }
#endif
}
