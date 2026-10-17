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

bool NvstusbController::start(const QString& firmware, float rate, int phaseMs,
                              bool invert, Trigger trigger, bool stats)
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
    _trigger = trigger;
    _stats = stats;
    _rate = rate;
    _clock.start();
    _windowStartNs = 0;
    _lastCallbackNs = -1;
    _deltaTotalNs = 0;
    _maxDeltaNs = 0;
    _maxUsbNs = 0;
    _callbackCount = 0;
    _intervalCount = 0;
    _lateCount = 0;
    _usbCommandCount = 0;
    _replacedCommands = 0;
    _lastEye = -1;
    _sameEyeCount = 0;

    std::fprintf(stderr,
            "bino: nvstusb: initialized at %.2f Hz, trigger %s, phase %d ms, invert %s, stats %s\n"
            "bino: nvstusb: Qt callbacks are NOT physical display presentation timestamps\n",
            rate, trigger == Trigger::Compose ? "compose" : "swap",
            phaseMs, invert ? "yes" : "no", stats ? "yes" : "no");
    return true;
#else
    Q_UNUSED(firmware);
    Q_UNUSED(rate);
    Q_UNUSED(phaseMs);
    Q_UNUSED(invert);
    Q_UNUSED(trigger);
    Q_UNUSED(stats);
    std::fprintf(stderr, "bino: nvstusb: this binary was built without ENABLE_NVSTUSB\n");
    return false;
#endif
}

bool NvstusbController::enabled() const
{
    return _ctx != nullptr;
}

bool NvstusbController::beforeComposition() const
{
    return _trigger == Trigger::Compose;
}

void NvstusbController::onFrameReady(int eye)
{
    if (!_ctx || (eye != 0 && eye != 1))
        return;

    if (_stats) {
        const qint64 now = _clock.nsecsElapsed();
        ++_callbackCount;
        if (_lastCallbackNs >= 0) {
            const qint64 delta = now - _lastCallbackNs;
            _deltaTotalNs += delta;
            ++_intervalCount;
            if (delta > _maxDeltaNs)
                _maxDeltaNs = delta;

            // Diagnostics only: Qt callback intervals >1.5 nominal refresh periods
            // are suspicious but do not prove a dropped physical display frame.
            if (double(delta) > (1.5e9 / double(_rate)))
                ++_lateCount;
        }
        _lastCallbackNs = now;
        if (_lastEye == eye)
            ++_sameEyeCount;
        _lastEye = eye;
        reportStatsIfDue(now);
    }

    if (_pendingEye >= 0)
        ++_replacedCommands;
    _pendingEye = eye;
    if (_phaseMs == 0) {
        _phaseTimer.stop();
        sendPendingEye();
    } else {
        // Phase delays use the latest eye; do not send an out-of-date command.
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

    const qint64 begin = _stats ? _clock.nsecsElapsed() : 0;
    const int error = nvstusb_send_eye(_ctx, eye == 0 ? nvstusb_left : nvstusb_right);
    if (_stats) {
        const qint64 duration = _clock.nsecsElapsed() - begin;
        if (duration > _maxUsbNs)
            _maxUsbNs = duration;
        if (error == 0)
            ++_usbCommandCount;
    }
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

void NvstusbController::reportStatsIfDue(qint64 now)
{
    if (now - _windowStartNs < 5000000000LL)
        return;

    const double elapsed = double(now - _windowStartNs) / 1e9;
    const double hz = elapsed > 0.0 ? double(_callbackCount) / elapsed : 0.0;
    const double avgDeltaMs = _intervalCount > 0
            ? double(_deltaTotalNs) / double(_intervalCount) / 1e6 : 0.0;

    std::fprintf(stderr,
            "bino: nvstusb stats: trigger=%s callbacks=%d (%.1f Hz) "
            "interval_avg=%.2fms max=%.2fms late(>1.5period)=%d "
            "same_eye=%d usb_sent=%d usb_max=%.2fms replaced=%d\n",
            _trigger == Trigger::Compose ? "compose" : "swap",
            _callbackCount, hz, avgDeltaMs, double(_maxDeltaNs) / 1e6,
            _lateCount, _sameEyeCount, _usbCommandCount,
            double(_maxUsbNs) / 1e6, _replacedCommands);

    _windowStartNs = now;
    _callbackCount = 0;
    _intervalCount = 0;
    _deltaTotalNs = 0;
    _maxDeltaNs = 0;
    _lateCount = 0;
    _sameEyeCount = 0;
    _usbCommandCount = 0;
    _maxUsbNs = 0;
    _replacedCommands = 0;
}
