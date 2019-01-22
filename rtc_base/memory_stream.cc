/*
 *  Copyright 2018 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <algorithm>

#include "rtc_base/memory_stream.h"

namespace rtc {

StreamState MemoryStream::GetState() const {
  return SS_OPEN;
}

StreamResult MemoryStream::Read(void* buffer,
                                size_t bytes,
                                size_t* bytes_read,
                                int* error) {
  if (seek_position_ >= data_length_) {
    return SR_EOS;
  }
  size_t available = data_length_ - seek_position_;
  if (bytes > available) {
    // Read partial buffer
    bytes = available;
  }
  memcpy(buffer, &buffer_[seek_position_], bytes);
  seek_position_ += bytes;
  if (bytes_read) {
    *bytes_read = bytes;
  }
  return SR_SUCCESS;
}

StreamResult MemoryStream::Write(const void* buffer,
                                 size_t bytes,
                                 size_t* bytes_written,
                                 int* error) {
  size_t available = buffer_length_ - seek_position_;
  if (0 == available) {
    // Increase buffer size to the larger of:
    // a) new position rounded up to next 256 bytes
    // b) double the previous length
    size_t new_buffer_length =
        std::max(((seek_position_ + bytes) | 0xFF) + 1, buffer_length_ * 2);
    StreamResult result = DoReserve(new_buffer_length, error);
    if (SR_SUCCESS != result) {
      return result;
    }
    RTC_DCHECK(buffer_length_ >= new_buffer_length);
    available = buffer_length_ - seek_position_;
  }

  if (bytes > available) {
    bytes = available;
  }
  memcpy(&buffer_[seek_position_], buffer, bytes);
  seek_position_ += bytes;
  if (data_length_ < seek_position_) {
    data_length_ = seek_position_;
  }
  if (bytes_written) {
    *bytes_written = bytes;
  }
  return SR_SUCCESS;
}

void MemoryStream::Close() {
  // nothing to do
}

bool MemoryStream::SetPosition(size_t position) {
  if (position > data_length_)
    return false;
  seek_position_ = position;
  return true;
}

bool MemoryStream::GetPosition(size_t* position) const {
  if (position)
    *position = seek_position_;
  return true;
}

bool MemoryStream::GetSize(size_t* size) const {
  if (size)
    *size = data_length_;
  return true;
}

bool MemoryStream::ReserveSize(size_t size) {
  return (SR_SUCCESS == DoReserve(size, nullptr));
}

///////////////////////////////////////////////////////////////////////////////

MemoryStream::MemoryStream() {}

MemoryStream::MemoryStream(const char* data) {
  SetData(data, strlen(data));
}

MemoryStream::MemoryStream(const void* data, size_t length) {
  SetData(data, length);
}

MemoryStream::~MemoryStream() {
  delete[] buffer_;
}

void MemoryStream::SetData(const void* data, size_t length) {
  data_length_ = buffer_length_ = length;
  delete[] buffer_;
  buffer_ = new char[buffer_length_];
  memcpy(buffer_, data, data_length_);
  seek_position_ = 0;
}

StreamResult MemoryStream::DoReserve(size_t size, int* error) {
  if (buffer_length_ >= size)
    return SR_SUCCESS;

  if (char* new_buffer = new char[size]) {
    memcpy(new_buffer, buffer_, data_length_);
    delete[] buffer_;
    buffer_ = new_buffer;
    buffer_length_ = size;
    return SR_SUCCESS;
  }

  if (error) {
    *error = ENOMEM;
  }
  return SR_ERROR;
}

}  // namespace rtc
