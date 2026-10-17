/*
 * Experimental NVIDIA 3D Vision emitter bridge for Bino.
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QElapsedTimer>

struct nvstusb_context;

/*
 * Best-effort Qt composition callbacks -> USB; NOT hardware vblank sync.
 * The USB writes currently execute on the GUI thread.
 */
class NvstusbController : public QObject
{
public:
    enum class Trigger { Compose, Swap };

    explicit NvstusbController(QObject* parent = nullptr);
    ~NvstusbController() override;

    bool start(const QString& firmware, float rate, int phaseMs, bool invert,
               Trigger trigger, bool stats, bool dryRun);
    bool enabled() const;
    void setIgnoreVideoWakeups(bool enabled) { _ignoreVideoWakeups = enabled; }
    bool ignoreVideoWakeups() const { return _ignoreVideoWakeups; }
    void noteSuppressedVideoWakeup();
    bool beforeComposition() const;
    void onFrameReady(int eye);
    void onFramePainted(int eye, bool newVideoFrameUploaded,
                        qint64 preRenderNs, qint64 paintCpuNs);
    void onQtComposing();
    void onQtSwapped();

private:
    void sendPendingEye();
    void reportStatsIfDue(qint64 now);

    nvstusb_context* _ctx = nullptr;
    bool _active = false;
    bool _dryRun = false;
    bool _ignoreVideoWakeups = false; // dry-run-only Qt scheduling experiment
    QTimer _phaseTimer;
    int _phaseMs = 0;
    int _pendingEye = -1;
    Trigger _trigger = Trigger::Swap;
    bool _stats = false;
    float _rate = 120.0f;

    QElapsedTimer _clock;
    qint64 _windowStartNs = 0;
    qint64 _lastCallbackNs = -1;
    qint64 _deltaTotalNs = 0;
    qint64 _maxDeltaNs = 0;
    qint64 _maxUsbNs = 0;
    int _callbackCount = 0;
    int _intervalCount = 0;
    int _lateCount = 0;
    int _usbCommandCount = 0;
    int _replacedCommands = 0;
    int _lastEye = -1;
    int _sameEyeCount = 0;
    // Time between alternating shutter requests, split by direction.
    qint64 _gapToEyeNs[2] = {0, 0};
    qint64 _gapToEyeMaxNs[2] = {0, 0};
    int _gapToEyeCount[2] = {0, 0};
    qint64 _usbByEyeMaxNs[2] = {0, 0};

    // Counts are observed Qt events, not compositor presentation feedback.
    int _paintCount = 0;
    int _composeCount = 0;
    int _swapCount = 0;
    int _paintsSinceCompose = 0;
    int _compositionsWithoutPaint = 0;
    int _coalescedPaints = 0;
    int _suppressedVideoWakeups = 0;
    int _samePaintEyeCount = 0;
    int _lastPaintEye = -1;
    qint64 _lastPaintNs = -1;
    qint64 _paintIntervalsNs = 0;
    qint64 _maxPaintIntervalNs = 0;
    int _paintIntervalCount = 0;
    // Distinguish expensive new video-frame uploads from cached-frame draws.
    int _paintTypeCount[2] = {0, 0}; // 0 = reused video frame, 1 = new upload
    qint64 _preRenderTypeTotalNs[2] = {0, 0};
    qint64 _preRenderTypeMaxNs[2] = {0, 0};
    qint64 _paintCpuTypeTotalNs[2] = {0, 0};
    qint64 _paintCpuTypeMaxNs[2] = {0, 0};
    int _paintLateTypeCount[2] = {0, 0};
};
