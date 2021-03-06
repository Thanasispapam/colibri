﻿/*
 * Copyright (c) 2020, EDEL LLC <http://www.edelsys.com>
 * All Rights Reserved
 *
 * Licensed under the MIT License;
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * https://opensource.org/licenses/MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "components/streamcomp.h"

#include <assert.h>

#include <muqueue/erqperiodic.h>

using namespace std;
using namespace fflow;

bool Stream::start() {
  if (!running_) {
    if (worker_) {
      fflow::AsyncERQPtr erq_handle = nullptr;

      fflow::add_periodic_handled<void>(([&](void) -> void {
                                          if (!worker_lock_.try_lock()) return;
                                          worker_(info_);
                                          worker_lock_.unlock();
                                        }),
                                        0, 1.0 / getStreamFramerate(),
                                        erq_handle);

      if (erq_handle) {
        assert(erq_handle_ == nullptr);
        erq_handle_ = erq_handle;
        running_ = true;
      }
    }
  }

  return running_;
}

void Stream::stop() {
  if (running_) {
    kill();
  }
}

void Stream::kill() {
  if (erq_handle_) {
    AsyncController *async_controller = nullptr;
    erq_handle_->removeFrom(async_controller);
    worker_lock_.unlock();
    erq_handle_ = nullptr;
  }
  running_ = false;
}

const StreamPtr MediaComponent::getStream(size_t idx) {
  StreamPtr result = nullptr;
  try {
    result = streams_.at(idx);
  } catch (const out_of_range &) {
    LOG(ERROR) << "No stream with index " << idx;
  }
  return result;
}

int MediaComponent::registerStream(const StreamInfo &stream_info,
                                   const Worker &worker) {
  StreamPtr stream = Stream::createStream(stream_info, worker);
  int result = -1;
  if (stream) {
    int idx = static_cast<int>(streams_.size());
    streams_.emplace_back(stream);
    result = idx;
  }
  return result;
}

int MediaComponent::registerStream(const StreamPtr stream) {
  int result = -1;
  if (stream) {
    int idx = static_cast<int>(streams_.size());
    streams_.emplace_back(stream);
    result = idx;
  }
  return result;
}

bool MediaComponent::startStream(uint8_t stream_id) {
  bool result = false;
  const StreamPtr stream = getStream(stream_id);
  if (stream) {
    if (onStartStream(stream)) {
      result = stream->start();
      if (result)
        LOG(INFO) << "Video stream with id=" << static_cast<int>(stream_id + 1)
                  << " has started";
    }
  }
  return result;
}

bool MediaComponent::stopStream(uint8_t stream_id) {
  bool result = false;
  const StreamPtr stream = getStream(stream_id);
  if (stream) {
    stream->stop();
    result = onStopStream(stream);
    if (result)
      LOG(INFO) << "Video stream with id=" << static_cast<int>(stream_id + 1)
                << " has stopped";
  }
  return result;
}

VideoServer::~VideoServer() {
  stop();

  // will remove from roster's component bus
  int compid = MAV_COMP_ID_CAMERA;
  while (compid <= MAV_COMP_ID_CAMERA6) {
    removeMediaComponent(compid);
    compid++;
  }
}

bool VideoServer::init(RouteSystemPtr roster) {
  if (roster == nullptr) return false;
  setRoster(roster);
  // add video protocol (only streaming is currently supported)
  getRoster()->add_protocol2(get_table(), get_table_len());
  return true;
}

bool VideoServer::start() {
  RouteSystemPtr roster = getRoster();
  if (!roster) {
    LOG(ERROR) << "Unable to start VideoServer - not initialized";
    return false;
  }
  if (n_inuse_ == 0) {
    LOG(ERROR) << "Unable to start VideoServer - no cameras added";
    return false;
  }
  // camera stream start will happen in handle_video_start_streaming
  return true;
}

void VideoServer::stop() {
  int compid = MAV_COMP_ID_CAMERA;

  while (compid <= MAV_COMP_ID_CAMERA6) {
    MediaComponentPtr mc = getMediaComponent(compid);
    if (mc) {
      size_t sz = mc->getNumberOfStreams();
      for (uint8_t idx = 0; idx < sz; ++idx) mc->stopStream(idx);
    }

    compid++;
  }
}

bool VideoServer::handle_request_camera_info(const mavlink_command_long_t &cmd,
                                             const fflow::SparseAddress &from) {
  bool result = false;

  if (is_zero(cmd.param1)) {
    send_ack(cmd.command, true, cmd.target_component, from.group_id,
             from.instance_id);
    return true;
  }

  MediaComponentPtr mc = getMediaComponent(cmd.target_component);
  if (mc) result = true;

  send_ack(cmd.command, result, cmd.target_component, from.group_id,
           from.instance_id);
  this_thread::sleep_for(chrono::milliseconds(100));

  if (result) {
    const MediaCapsInfo &mcinfo = mc->getCapsInfo();
    mavlink_message_t msg;

    mavlink_msg_camera_information_pack(
        getRoster()->getMcastId(), cmd.target_component, &msg,
        uint32_t(1000 * mcinfo.time_boot_ms),
        reinterpret_cast<const uint8_t *>(mcinfo.vendor_name.c_str()),
        reinterpret_cast<const uint8_t *>(mcinfo.model_name.c_str()),
        mcinfo.firmware_version, mcinfo.focal_length, mcinfo.sensor_size_h,
        mcinfo.sensor_size_v, mcinfo.resolution_h, mcinfo.resolution_v,
        uint8_t(mcinfo.lens_id), mcinfo.flags, mcinfo.cam_definition_version,
        mcinfo.cam_definition_uri.c_str());

    send_mavlink_message(msg, cmd.target_component, from.group_id,
                         from.instance_id);
  }

  return result;
}

bool VideoServer::handle_request_video_stream_info(
    const mavlink_command_long_t &cmd, const fflow::SparseAddress &from) {
  bool result = false;

  MediaComponent *mc = getMediaComponent(cmd.target_component);
  if (mc) {
    uint8_t sz = static_cast<uint8_t>(mc->getNumberOfStreams());
    uint8_t stream_id = static_cast<uint8_t>(std::abs(cmd.param1));

    if (stream_id > 0) {
      const StreamPtr stream = mc->getStream(--stream_id);
      if (stream) result = true;

      send_ack(cmd.command, result, cmd.target_component, from.group_id,
               from.instance_id);
      this_thread::sleep_for(chrono::milliseconds(100));

      if (result) {
        mavlink_message_t msg;

        mavlink_msg_video_stream_information_pack(
            getRoster()->getMcastId(), cmd.target_component, &msg, stream_id,
            sz, stream->getStreamType(), stream->getStreamFlags(),
            stream->getStreamFramerate(), stream->getStreamWidth(),
            stream->getStreamHeight(), stream->getStreamBitrate(),
            stream->getStreamRotation(), stream->getStreamHFov(),
            stream->getStreamName().c_str(), stream->getStreamURI().c_str());

        send_mavlink_message(msg, cmd.target_component, from.group_id,
                             from.instance_id);
      }
    } else {
      if (sz > 0) result = true;

      send_ack(cmd.command, result, cmd.target_component, from.group_id,
               from.instance_id);
      this_thread::sleep_for(chrono::milliseconds(100));

      if (result) {
        for (uint8_t i = 0; i < sz; ++i) {
          const StreamPtr stream = mc->getStream(i);
          if (stream) {
            uint8_t stream_id = static_cast<uint8_t>(i + 1);
            mavlink_message_t msg;

            mavlink_msg_video_stream_information_pack(
                getRoster()->getMcastId(), cmd.target_component, &msg,
                stream_id, sz, stream->getStreamType(),
                stream->getStreamFlags(), stream->getStreamFramerate(),
                stream->getStreamWidth(), stream->getStreamHeight(),
                stream->getStreamBitrate(), stream->getStreamRotation(),
                stream->getStreamHFov(), stream->getStreamName().c_str(),
                stream->getStreamURI().c_str());

            send_mavlink_message(msg, cmd.target_component, from.group_id,
                                 from.instance_id);
            this_thread::sleep_for(chrono::milliseconds(100));
          }
        }
      }
    }
  } else {
    send_ack(cmd.command, false, cmd.target_component, from.group_id,
             from.instance_id);
  }

  return result;
}

bool VideoServer::handle_video_start_streaming(
    const mavlink_command_long_t &cmd, const SparseAddress &from) {
  bool result = false;
  int compid = cmd.target_component;

  MediaComponent *mc = getMediaComponent(compid);
  if (mc && mc->getNumberOfStreams() > 0) result = true;

  send_ack(cmd.command, result, cmd.target_component, from.group_id,
           from.instance_id);
  this_thread::sleep_for(chrono::milliseconds(100));

  if (result) {
    uint8_t stream_id = static_cast<uint8_t>(std::abs(cmd.param1));

    if (stream_id > 0) {
      result = mc->startStream(--stream_id);
    } else {
      uint8_t sz = static_cast<uint8_t>(mc->getNumberOfStreams());
      for (uint8_t i = 0; i < sz; ++i) {
        const StreamPtr stream = mc->getStream(i);
        if (stream) result &= mc->startStream(i);
      }
    }
  }

  return result;
}

bool VideoServer::handle_video_stop_streaming(const mavlink_command_long_t &cmd,
                                              const SparseAddress &from) {
  bool result = false;
  int compid = cmd.target_component;

  MediaComponent *mc = getMediaComponent(compid);
  if (mc) result = true;

  send_ack(cmd.command, result, cmd.target_component, from.group_id,
           from.instance_id);
  this_thread::sleep_for(chrono::milliseconds(100));

  if (result) {
    uint8_t stream_id = static_cast<uint8_t>(std::abs(cmd.param1));

    if (stream_id > 0) {
      result = mc->stopStream(--stream_id);
    } else {
      uint8_t sz = static_cast<uint8_t>(mc->getNumberOfStreams());
      for (uint8_t i = 0; i < sz; ++i) {
        const StreamPtr stream = mc->getStream(i);
        if (stream) result &= mc->stopStream(i);
      }
    }
  }

  return result;
}

pointprec_t VideoServer::command_long_handler(uint8_t *payload, size_t /*len*/,
                                              SparseAddress from) {
  mavlink_message_t *msg = MAVPAYLOAD_TO_MAVMSG(payload);
  mavlink_command_long_t cmd;

  mavlink_msg_command_long_decode(msg, &cmd);

  uint8_t comp_id = cmd.target_component;
  if (comp_id < MAV_COMP_ID_CAMERA || comp_id > MAV_COMP_ID_CAMERA6) {
    // comp_id not related to any camera, just exit silently
    return 1.0;
  }

  RouteSystemPtr roster = getRoster();
  if (!roster) {
    LOG(ERROR) << "VideoServer is not initialized!";
    return 1.0;
  }

  LOG(INFO) << "Command received: (sysid:"
            << static_cast<int>(cmd.target_system)
            << " compid:" << static_cast<int>(comp_id)
            << " msgid:" << static_cast<int>(cmd.command) << ")";

  bool result = false;

  if (!roster->getCBus().has_component(comp_id)) {
    LOG(INFO) << "Camera with ID=" << comp_id << " not present";
  } else {
    switch (cmd.command) {
      case MAV_CMD_REQUEST_CAMERA_INFORMATION:
        result = handle_request_camera_info(cmd, from);
        break;
      case MAV_CMD_REQUEST_VIDEO_STREAM_INFORMATION:
        result = handle_request_video_stream_info(cmd, from);
        break;
      case MAV_CMD_VIDEO_START_STREAMING:
        result = handle_video_start_streaming(cmd, from);
        break;
      case MAV_CMD_VIDEO_STOP_STREAMING:
        result = handle_video_stop_streaming(cmd, from);
        break;
      // NI
      case MAV_CMD_REQUEST_CAMERA_SETTINGS:
      case MAV_CMD_SET_CAMERA_MODE:
      case MAV_CMD_REQUEST_CAMERA_CAPTURE_STATUS:
      case MAV_CMD_RESET_CAMERA_SETTINGS:
      case MAV_CMD_REQUEST_STORAGE_INFORMATION:
      case MAV_CMD_STORAGE_FORMAT:
      case MAV_CMD_IMAGE_START_CAPTURE:
      case MAV_CMD_IMAGE_STOP_CAPTURE:
      case MAV_CMD_VIDEO_START_CAPTURE:
      case MAV_CMD_VIDEO_STOP_CAPTURE:
      case MAV_CMD_REQUEST_CAMERA_IMAGE_CAPTURE:
      case MAV_CMD_DO_TRIGGER_CONTROL:
        send_ack(cmd.command, false, cmd.target_component, from.group_id,
                 from.instance_id);
        LOG(INFO) << "Camera command " << static_cast<int>(cmd.command)
                  << " not supported";
        break;
      default:
        LOG(INFO) << "Command " << static_cast<int>(cmd.command)
                  << " unhandled";
        break;
    }
  }

  return 1.0;
}

bool VideoServer::addMediaComponent(MediaComponentPtr comp) {
  int compid = MAV_COMP_ID_CAMERA;
  RouteSystemPtr roster = getRoster();
  bool ret = false;

  if (roster && comp) {
    while (compid <= MAV_COMP_ID_CAMERA6) {
      if (!roster->getCBus().has_component(compid)) {
        comp->setId(static_cast<uint8_t>(compid));
        comp->setRoster(roster);
        ret = roster->getCBus().add_component(comp);
        if (ret) ++n_inuse_;
        break;
      }
      compid++;
    }
  }

  return ret;
}

bool VideoServer::removeMediaComponent(int comp_id) {
  bool result = false;
  RouteSystemPtr roster = getRoster();
  if (roster && comp_id >= MAV_COMP_ID_CAMERA &&
      comp_id <= MAV_COMP_ID_CAMERA6) {
    if (roster->getCBus().remove_component(comp_id)) {
      --n_inuse_;
      result = true;
    }
  }
  return result;
}

bool VideoServer::removeMediaComponent(MediaComponentPtr comp) {
  RouteSystemPtr roster = getRoster();
  if (roster && comp) return removeMediaComponent(comp->getId());
  return false;
}

MediaComponentPtr VideoServer::getMediaComponent(int comp_id) {
  if (comp_id < MAV_COMP_ID_CAMERA || comp_id > MAV_COMP_ID_CAMERA6 ||
      !getRoster())
    return nullptr;
  return dynamic_cast<MediaComponentPtr>(
      getRoster()->getCBus().get_component(comp_id));
}
