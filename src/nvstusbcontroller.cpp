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
                              bool invert, Trigger trigger, bool stats, bool dryRun)
{
    if (_active || rate <= 60.0f || phaseMs < 0 || phaseMs > 7)
        return false;

#ifdef WITH_NVSTUSB
    if (!dryRun) {
        const QByteArray file = firmware.toLocal8Bit();
        _ctx = nvstusb_init(file.isEmpty() ? nullptr : file.constData());
        if (!_ctx) {
            std::fprintf(stderr, "bino: nvstusb: unable to initialize USB emitter\n");
            return false;
        }

        nvstusb_set_rate(_ctx, rate);
        const int error = nvstusb_last_error(_ctx);
        if (error != 0) {
            std::fprintf(stderr,
                    "bino: nvstusb: failed to program refresh rate (USB %d)\n", error);
            nvstusb_deinit(_ctx);
            _ctx = nullptr;
            return false;
        }
        if (invert)
            nvstusb_invert_eyes(_ctx);
    }
#else
    Q_UNUSED(firmware);
    Q_UNUSED(invert);
    if (!dryRun) {
        std::fprintf(stderr,
                "bino: nvstusb: this binary was built without ENABLE_NVSTUSB\n");
        return false;
    }
#endif

    _active = true;
    _dryRun = dryRun;
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
    _paintCount = 0;
    _composeCount = 0;
    _swapCount = 0;
    _paintsSinceCompose = 0;
    _compositionsWithoutPaint = 0;
    _coalescedPaints = 0;
    _samePaintEyeCount = 0;
    _lastPaintEye = -1;
    _lastPaintNs = -1;
    _paintIntervalsNs = 0;
    _maxPaintIntervalNs = 0;
    _paintIntervalCount = 0;

    std::fprintf(stderr,
            "bino: nvstusb: %s at %.2f Hz, trigger %s, phase %d ms, "
            "invert %s, stats %s\n"
            "bino: nvstusb: Qt paint/compose/swap events are NOT hardware "
            "presentation timestamps\n",
            dryRun ? "DRY RUN (no USB opened)" : "initialized",
            rate, trigger == Trigger::Compose ? "compose" : "swap",
            phaseMs, invert ? "yes" : "no", stats ? "yes" : "no");
    return true;
}

bool NvstusbController::enabled() const
{
    return _active;
}

bool NvstusbController::beforeComposition() const
{
    return _trigger == Trigger::Compose;
}

void NvstusbController::onFramePainted(int eye)
{
    if (!_active || !_stats || (eye != 0 && eye != 1))
        return;
    const qint64 now = _clock.nsecsElapsed();
    ++_paintCount;
    ++_paintsSinceCompose;
    if (_lastPaintNs >= 0) {
        const qint64 delta = now - _lastPaintNs;
        _paintIntervalsNs += delta;
        ++_paintIntervalCount;
        if (delta > _maxPaintIntervalNs)
            _maxPaintIntervalNs = delta;
    }
    _lastPaintNs = now;
    if (_lastPaintEye == eye)
        ++_samePaintEyeCount;
    _lastPaintEye = eye;
}

void NvstusbController::onQtComposing()
{
    if (!_active || !_stats)
        return;
    ++_composeCount;
    if (_paintsSinceCompose == 0)
        ++_compositionsWithoutPaint;
    if (_paintsSinceCompose > 1)
        _coalescedPaints += _paintsSinceCompose - 1;
    _paintsSinceCompose = 0;
}

void NvstusbController::onQtSwapped()
{
    if (!_active || !_stats)
        return;
    ++_swapCount;
    reportStatsIfDue(_clock.nsecsElapsed());
}

void NvstusbController::onFrameReady(int eye)
{
    if (!_active || (eye != 0 && eye != 1))
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

            // Intervals >1.5 nominal refresh periods are suspicious, but
            // cannot prove a missed hardware presentation.
            if (double(delta) > (1.5e9 / double(_rate)))
                ++_lateCount;
        }
        _lastCallbackNs = now;
        if (_lastEye == eye)
            ++_sameEyeCount;
        _lastEye = eye;
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
    if (!_active || _pendingEye < 0)
        return;

    const int eye = _pendingEye;
    _pendingEye = -1;

    if (_dryRun) {
        if (_stats)
            ++_usbCommandCount; // Simulated command, no USB transfer.
        return;
    }

#ifdef WITH_NVSTUSB
    if (!_ctx)
        return;

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
        _active = false;
    }
#else
    Q_UNUSED(eye);
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
    const double paintHz = elapsed > 0.0 ? double(_paintCount) / elapsed : 0.0;
    const double composeHz = elapsed > 0.0 ? double(_composeCount) / elapsed : 0.0;
    const double swapHz = elapsed > 0.0 ? double(_swapCount) / elapsed : 0.0;
    const double paintAvgMs = _paintIntervalCount > 0
            ? double(_paintIntervalsNs) / double(_paintIntervalCount) / 1e6 : 0.0;

    std::fprintf(stderr,
            "bino: nvstusb stats: trigger=%s%s "
            "paint=%d(%.1fHz avg=%.2fms max=%.2fms same_eye=%d) "
            "compose=%d(%.1fHz no_new_paint=%d coalesced=%d) "
            "swap=%d(%.1fHz) callbacks=%d(%.1fHz avg=%.2fms max=%.2fms "
            "late=%d same_eye=%d) commands=%d usb_max=%.2fms replaced=%d\n",
            _trigger == Trigger::Compose ? "compose" : "swap",
            _dryRun ? "/dry-run" : "",
            _paintCount, paintHz, paintAvgMs, double(_maxPaintIntervalNs) / 1e6,
            _samePaintEyeCount, _composeCount, composeHz,
            _compositionsWithoutPaint, _coalescedPaints,
            _swapCount, swapHz, _callbackCount, hz, avgDeltaMs,
            double(_maxDeltaNs) / 1e6, _lateCount, _sameEyeCount,
            _usbCommandCount, double(_maxUsbNs) / 1e6, _replacedCommands);

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
    _paintCount = 0;
    _composeCount = 0;
    _swapCount = 0;
    _compositionsWithoutPaint = 0;
    _coalescedPaints = 0;
    _samePaintEyeCount = 0;
    _paintIntervalsNs = 0;
    _maxPaintIntervalNs = 0;
    _paintIntervalCount = 0;
}
