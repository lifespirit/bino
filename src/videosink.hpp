/*
 * This file is part of Bino, a 3D video player.
 *
 * Copyright (C) 2022, 2023, 2024, 2025
 * Martin Lambers <marlam@marlam.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <QVideoSink>
#include <QThread>
#include <optional>

#include "modes.hpp"
#include "videoframe.hpp"
#include "metadata.hpp"
#include "playlist.hpp"


class VideoSink : public QVideoSink
{
Q_OBJECT

public:
    unsigned long long frameCounter; // number of frames seen for this URL
    VideoFrame* frame;    // target video frame
    VideoFrame* extFrame; // extension to target video frame, for alternating stereo
    bool *frameIsNew;     // flag to set when the target frame represents a new frame
    bool needExtFrame;    // flag to set in alternating stereo when extFrame is not filled yet
    InputMode inputMode;  // input mode of current media
    SurroundMode surroundMode; // surround mode of the current media
    bool lastFrameWasValid; // last frame of current media was valid

    VideoSink(VideoFrame* frame, VideoFrame* extFrame, bool* frameIsNew);
    ~VideoSink() override;

    void newPlaylistEntry(const PlaylistEntry& entry, const MetaData& metaData);

public Q_SLOTS:
    void processNewFrame(const QVideoFrame& frame);

signals:
    void newVideoFrame();

private:
    // A bounded, latest-frame-only worker used only with BINO_ASYNC_VIDEO_MAP=1.
    // Only the GUI thread reads/writes the currently displayed VideoFrame.
    struct AsyncFrameRequest {
        QVideoFrame frame;
        InputMode inputMode;
        SurroundMode surroundMode;
        bool newSource;
        quint64 generation;
    };
    void dispatchAsyncFrame(AsyncFrameRequest request);
    bool _asyncMap = false;
    QThread _asyncThread;
    QObject* _asyncWorker = nullptr; // owned by _asyncThread, deleted on finished()
    bool _asyncBusy = false;
    std::optional<AsyncFrameRequest> _asyncPending;
    quint64 _asyncGeneration = 0;
    quint64 _asyncSkipped = 0;
    quint64 _asyncDelivered = 0;

    // Optional wall-clock profiling of the videoFrameChanged handler.
    // This runs before the next paintGL(), so its time is not included
    // in the existing Bino/NVSTUSB paint timing measurements.
    bool _profileFrameProcessing = false;
    int _processSamples = 0;
    int _processSlow1ms = 0;
    int _processSlow4ms = 0;
    int _processSlow8ms = 0;
    qint64 _processTotalNs = 0;
    qint64 _processMaxNs = 0;
    qint64 _updateTotalNs = 0;
    qint64 _updateMaxNs = 0;
};
