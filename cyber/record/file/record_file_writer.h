/******************************************************************************
 * Copyright 2018 The Apollo Authors. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *****************************************************************************/

#ifndef CYBER_RECORD_FILE_RECORD_FILE_WRITER_H_
#define CYBER_RECORD_FILE_RECORD_FILE_WRITER_H_

#include <condition_variable>
#include <fstream>
#include <future>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

#include "google/protobuf/io/zero_copy_stream_impl.h"
#include "google/protobuf/message.h"
#include "google/protobuf/text_format.h"

#include "cyber/common/log.h"
#include "cyber/record/file/record_file_base.h"
#include "cyber/record/file/section.h"
#include "cyber/time/time.h"

namespace apollo {
namespace cyber {
namespace record {

struct Chunk {
  Chunk() { clear(); }

  inline void clear() {
    body_.reset(new proto::ChunkBody());
    header_.set_begin_time(0);
    header_.set_end_time(0);
    header_.set_message_number(0);
    header_.set_raw_size(0);
  }

  inline void add(const proto::SingleMessage& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    proto::SingleMessage* p_message = body_->add_messages();
    *p_message = message;
    if (header_.begin_time() == 0) {
      header_.set_begin_time(message.time());
    }
    if (header_.begin_time() > message.time()) {
      header_.set_begin_time(message.time());
    }
    if (header_.end_time() < message.time()) {
      header_.set_end_time(message.time());
    }
    header_.set_message_number(header_.message_number() + 1);
    header_.set_raw_size(header_.raw_size() + message.content().size());
  }

  inline bool empty() { return header_.message_number() == 0; }

  std::mutex mutex_;
  proto::ChunkHeader header_;
  std::unique_ptr<proto::ChunkBody> body_ = nullptr;
};

/**
Writes cyber record files on an asynchronous task
*/
class RecordFileWriter : public RecordFileBase {
 public:
  RecordFileWriter() = default;
  ~RecordFileWriter();
  bool Open(const std::string& path) override;
  void Close() override;
  bool WriteHeader(const proto::Header& header);
  bool WriteChannel(const proto::Channel& channel);
  bool WriteMessage(const proto::SingleMessage& message);
  uint64_t GetMessageNumber(const std::string& channel_name) const;

  // For testing
  void WaitForWrite();

 private:
  bool WriteChunk(const proto::ChunkHeader& chunk_header,
                  const proto::ChunkBody& chunk_body);
  template <typename T>
  bool WriteSection(const T& message);
  bool WriteIndex();
  void Flush(const Chunk& chunk);
  bool IsChunkFlushEmpty();
  void BlockUntilSpaceAvailable();
  // make moveable
  std::unique_ptr<Chunk> chunk_active_;
  // Initialize with a dummy value to simplify checking later
  std::future<void> flush_task_ = std::async(std::launch::async, []() {});
  std::unordered_map<std::string, uint64_t> channel_message_number_map_;
};

template <typename T>
bool RecordFileWriter::WriteSection(const T& message) {
  proto::SectionType type;
  // is_same<X,Y>::value用于判断X和Y是否是同一类型
  if (std::is_same<T, proto::ChunkHeader>::value) {
    type = proto::SectionType::SECTION_CHUNK_HEADER;
  } else if (std::is_same<T, proto::ChunkBody>::value) {
    type = proto::SectionType::SECTION_CHUNK_BODY;
  } else if (std::is_same<T, proto::Channel>::value) {
    type = proto::SectionType::SECTION_CHANNEL;
  } else if (std::is_same<T, proto::Header>::value) {
    type = proto::SectionType::SECTION_HEADER;
    // Header要写2次，第1次不全，所有数据写完后更新header，再写1次
    if (!SetPosition(0)) {
      AERROR << "Jump to position #0 failed";
      return false;
    }
  } else if (std::is_same<T, proto::Index>::value) {
    type = proto::SectionType::SECTION_INDEX;
  } else {
    AERROR << "Do not support this template typename.";
    return false;
  }
  Section section;
  /// zero out whole struct even if padded
  memset(&section, 0, sizeof(section));
  section.type = type;
  section.size = static_cast<int64_t>(message.ByteSizeLong());
  // 先写Section的类型和大小
  ssize_t count = write(fd_, &section, sizeof(section));
  if (count < 0) {
    AERROR << "Write fd failed, fd: " << fd_ << ", errno: " << errno;
    return false;
  }
  if (count != sizeof(section)) {
    AERROR << "Write fd failed, fd: " << fd_
           << ", expect count: " << sizeof(section)
           << ", actual count: " << count;
    return false;
  }
  {
    //写Secton的内容--zero copy方式
    google::protobuf::io::FileOutputStream raw_output(fd_);
    message.SerializeToZeroCopyStream(&raw_output);
  }
  if (type == proto::SectionType::SECTION_HEADER) {
    // 若是section header则补齐到2048bytes
    static char blank[HEADER_LENGTH] = {'0'};
    count = write(fd_, &blank, HEADER_LENGTH - message.ByteSizeLong());
    if (count < 0) {
      AERROR << "Write fd failed, fd: " << fd_ << ", errno: " << errno;
      return false;
    }
    if (static_cast<size_t>(count) != HEADER_LENGTH - message.ByteSizeLong()) {
      AERROR << "Write fd failed, fd: " << fd_
             << ", expect count: " << sizeof(section)
             << ", actual count: " << count;
      return false;
    }
  }
  header_.set_size(CurrentPosition());
  return true;
}

}  // namespace record
}  // namespace cyber
}  // namespace apollo

#endif  // CYBER_RECORD_FILE_RECORD_FILE_WRITER_H_
