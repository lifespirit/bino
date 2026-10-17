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

#include <QUrl>
#include <QElapsedTimer>
#include <QCoreApplication>
#include <QThread>
#include <QtGlobal>
#include <cstdio>
#include <memory>
#include <utility>

#include "videosink.hpp"
#include "log.hpp"


VideoSink::VideoSink(VideoFrame* frame, VideoFrame* extFrame, bool* frameIsNew) :
    frameCounter(0),
    frame(frame),
    extFrame(extFrame),
    frameIsNew(frameIsNew),
    needExtFrame(false),
    inputMode(Input_Unknown),
    surroundMode(Surround_Unknown),
    lastFrameWasValid(false)
{
    _profileFrameProcessing = qEnvironmentVariableIntValue("BINO_PROFILE_VIDEO_SINK") != 0;
    _asyncMap = qEnvironmentVariableIntValue("BINO_ASYNC_VIDEO_MAP") != 0;
    if (_asyncMap) {
        _asyncWorker = new QObject;
        _asyncWorker->moveToThread(&_asyncThread);
        connect(&_asyncThread, &QThread::finished, _asyncWorker, &QObject::deleteLater);
        _asyncThread.start();
        std::fprintf(stderr,
                "bino: video_sink: EXPERIMENTAL async QVideoFrame mapping enabled; "
                "dropping stale frames when the worker is busy\n");
    }
    if (_profileFrameProcessing)
        std::fprintf(stderr,
                "bino: video_sink profile enabled (measures processNewFrame(), before paintGL)\n");
    connect(this, SIGNAL(videoFrameChanged(const QVideoFrame&)), this, SLOT(processNewFrame(const QVideoFrame&)));
}

VideoSink::~VideoSink()
{
    if (_asyncWorker) {
        _asyncThread.quit();
        _asyncThread.wait();
        // QObject::deleteLater is handled by QThread::finished; no
        // worker can refer to this VideoSink after wait() returns.
    }
}

// Prepare video pixels off the GUI thread. The mapped pixel pointers
// cannot outlive QVideoFrame::unmap(), so we copy them before posting the
// completed frame back to the event loop.
void VideoSink::dispatchAsyncFrame(AsyncFrameRequest request)
{
    Q_ASSERT(_asyncWorker && !_asyncBusy);
    _asyncBusy = true;
    const auto job = std::make_shared<AsyncFrameRequest>(std::move(request));
    QMetaObject::invokeMethod(_asyncWorker,
            [this, job]() {
        auto prepared = std::make_shared<VideoFrame>();
        prepared->update(job->inputMode, job->surroundMode,
                job->frame, job->newSource);
        bool usable = true;
        if (prepared->storage == VideoFrame::Storage_Mapped) {
            usable = prepared->qframe.isMapped();
            if (usable) {
                for (int p = 0; p < prepared->planeCount; ++p) {
                    const uchar* data = prepared->mappedBits[p];
                    const int n = prepared->bytesPerPlane[p];
                    if (!data || n <= 0) {
                        usable = false;
                        break;
                    }
                    prepared->bits[p].assign(data, data + n);
                }
            }
            if (prepared->qframe.isMapped())
                prepared->qframe.unmap();
            for (int p = 0; p < 3; ++p)
                prepared->mappedBits[p] = nullptr;
            if (usable)
                prepared->storage = VideoFrame::Storage_Copied;
        }

        QMetaObject::invokeMethod(this,
                [this, prepared, usable, generation = job->generation]() mutable {
            if (generation == _asyncGeneration && usable) {
                if (frame->qframe.isMapped())
                    frame->qframe.unmap();
                *frame = std::move(*prepared);
                extFrame->invalidate();
                *frameIsNew = true;
                ++_asyncDelivered;
                if (_asyncDelivered % 120 == 0) {
                    std::fprintf(stderr,
                            "bino: video_sink async stats: delivered_total=%llu "
                            "overwritten_pending_since_last=%llu "
                            "gui_thread=%s\n",
                            static_cast<unsigned long long>(_asyncDelivered),
                            static_cast<unsigned long long>(_asyncSkipped),
                            QCoreApplication::instance()
                                && QThread::currentThread() == QCoreApplication::instance()->thread()
                                    ? "yes" : "no");
                    _asyncSkipped = 0;
                }
                emit newVideoFrame();
            } else if (generation == _asyncGeneration && !usable) {
                std::fprintf(stderr,
                        "bino: video_sink: async frame mapping failed, keeping previous frame\n");
            }

            _asyncBusy = false;
            if (_asyncPending) {
                AsyncFrameRequest next = std::move(*_asyncPending);
                _asyncPending.reset();
                dispatchAsyncFrame(std::move(next));
            }
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

// called whenever new media is played:
void VideoSink::newPlaylistEntry(const PlaylistEntry& entry, const MetaData& metaData)
{
    frameCounter = 0;
    lastFrameWasValid = false;
    // The in-flight frame can finish, but its generation will not match.
    ++_asyncGeneration;
    _asyncPending.reset();

    int vt = entry.videoTrack;
    if (vt < 0)
        vt = 0;

    inputMode = entry.inputMode;
    if (inputMode == Input_Unknown) {
        if (vt < metaData.inputModes.size())
            inputMode = metaData.inputModes[vt];
    }
    LOG_DEBUG("input mode for %s: %s", qPrintable(entry.url.toString()), inputModeToString(inputMode));
    surroundMode = entry.surroundMode;
    if (surroundMode == Surround_Unknown) {
        if (vt < metaData.surroundModes.size())
            surroundMode = metaData.surroundModes[vt];
    }
    LOG_DEBUG("surround mode for %s: %s", qPrintable(entry.url.toString()), surroundModeToString(surroundMode));
}

void VideoSink::processNewFrame(const QVideoFrame& frame)
{
    QElapsedTimer profileClock;
    if (_profileFrameProcessing)
        profileClock.start();

    if (!frame.isValid() && lastFrameWasValid) {
        // keep showing the last frame of the current media; ignore that QtMultiMedia
        // with the FFmpeg backend gives us an invalid frame as soon as the media stops
        // (it does not do this with the GStreamer backend)
        LOG_DEBUG("video sink gets invalid frame and ignores it since last frame of current media was valid");
        return;
    }
    if (frame.isValid()) {
        LOG_FIREHOSE("video sink gets a valid frame");
        lastFrameWasValid = true;
    }

    // In normal SBS/TB modes, only one decoded frame represents both eyes.
    // The original two-frame input-alternating mode is deliberately not
    // modified by this experiment.
    if (_asyncMap && frame.isValid()
            && inputMode != Input_Alternating_LR
            && inputMode != Input_Alternating_RL) {
        AsyncFrameRequest request{frame, inputMode, surroundMode,
                                  frameCounter == 0, _asyncGeneration};
        ++frameCounter;
        if (_asyncBusy) {
            if (_asyncPending)
                ++_asyncSkipped; // an older not-yet-started frame is replaced
            _asyncPending = std::move(request);
        } else {
            dispatchAsyncFrame(std::move(request));
        }
        return;
    }

    bool updateExtFrame;
    if (inputMode == Input_Alternating_LR || inputMode == Input_Alternating_RL) {
        if (needExtFrame) {
            LOG_FIREHOSE("video sink updates extended frame for alternating mode");
            updateExtFrame = true;
            needExtFrame = false;
        } else {
            LOG_FIREHOSE("video sink updates standard frame for alternating mode");
            updateExtFrame = false;
            needExtFrame = true;
        }
    } else {
        LOG_FIREHOSE("video sink updates standard frame for non-alternating mode");
        updateExtFrame = false;
        needExtFrame = false;
    }
    const qint64 updateStartNs = _profileFrameProcessing ? profileClock.nsecsElapsed() : 0;
    if (updateExtFrame) {
        this->extFrame->update(inputMode, surroundMode, frame, frameCounter == 0);
    } else {
        this->frame->update(inputMode, surroundMode, frame, frameCounter == 0);
        this->extFrame->invalidate();
    }
    const qint64 updateNs = _profileFrameProcessing
            ? profileClock.nsecsElapsed() - updateStartNs : 0;
    if (!needExtFrame) {
        LOG_FIREHOSE("video sink signals that new frame is complete");
        *frameIsNew = true;
        emit newVideoFrame();
    }
    frameCounter++;

    if (_profileFrameProcessing) {
        const qint64 elapsedNs = profileClock.nsecsElapsed();
        ++_processSamples;
        _processTotalNs += elapsedNs;
        _updateTotalNs += updateNs;
        if (elapsedNs > _processMaxNs)
            _processMaxNs = elapsedNs;
        if (updateNs > _updateMaxNs)
            _updateMaxNs = updateNs;
        if (elapsedNs > 1000000LL)
            ++_processSlow1ms;
        if (elapsedNs > 4000000LL)
            ++_processSlow4ms;
        if (elapsedNs > 8000000LL)
            ++_processSlow8ms;

        if (_processSamples >= 120) {
            std::fprintf(stderr,
                    "bino: video_sink stats: gui_thread=%s frames=%d "
                    "handler_avg=%.3fms handler_max=%.3fms "
                    "update_avg=%.3fms update_max=%.3fms "
                    "handler_gt1ms=%d gt4ms=%d gt8ms=%d\n",
                    QCoreApplication::instance()
                        && QThread::currentThread() == QCoreApplication::instance()->thread()
                            ? "yes" : "no",
                    _processSamples,
                    double(_processTotalNs) / _processSamples / 1e6,
                    double(_processMaxNs) / 1e6,
                    double(_updateTotalNs) / _processSamples / 1e6,
                    double(_updateMaxNs) / 1e6,
                    _processSlow1ms, _processSlow4ms, _processSlow8ms);
            _processSamples = 0;
            _processSlow1ms = 0;
            _processSlow4ms = 0;
            _processSlow8ms = 0;
            _processTotalNs = 0;
            _processMaxNs = 0;
            _updateTotalNs = 0;
            _updateMaxNs = 0;
        }
    }
}
