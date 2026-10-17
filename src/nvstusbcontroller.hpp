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
               Trigger trigger, bool stats);
    bool enabled() const;
    bool beforeComposition() const;
    void onFrameReady(int eye);

private:
    void sendPendingEye();
    void reportStatsIfDue(qint64 now);

    nvstusb_context* _ctx = nullptr;
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
};
