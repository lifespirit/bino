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

struct nvstusb_context;

/*
 * This is a best-effort frameSwapped->USB bridge, NOT hardware vblank sync.
 * All calls take place on the Qt GUI thread in this first prototype.
 */
class NvstusbController : public QObject
{
public:
    explicit NvstusbController(QObject* parent = nullptr);
    ~NvstusbController() override;

    bool start(const QString& firmware, float rate, int phaseMs, bool invert);
    bool enabled() const;
    void onFrameSwapped(int eye);

private:
    void sendPendingEye();

    nvstusb_context* _ctx = nullptr;
    QTimer _phaseTimer;
    int _phaseMs = 0;
    int _pendingEye = -1;
};
