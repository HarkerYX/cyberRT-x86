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

#include "cyber/transport/shm/posix_segment.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "cyber/common/log.h"
#include "cyber/common/util.h"
#include "cyber/transport/shm/block.h"
#include "cyber/transport/shm/segment.h"
#include "cyber/transport/shm/shm_conf.h"

namespace apollo {
namespace cyber {
namespace transport {

PosixSegment::PosixSegment(uint64_t channel_id) : Segment(channel_id) {
  shm_name_ = std::to_string(channel_id);
}

PosixSegment::~PosixSegment() { Destroy(); }

bool PosixSegment::OpenOrCreate() {
  if (init_) {
    return true;
  }

  // create managed_shm_  创建或者打开共享内存文件
  int fd = shm_open(shm_name_.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
  if (fd < 0) {
    if (EEXIST == errno) {
      ADEBUG << "shm already exist, open only.";
      return OpenOnly();
    } else {
      AERROR << "create shm failed, error: " << strerror(errno);
      return false;
    }
  }

 // 重置文件大小
  if (ftruncate(fd, conf_.managed_shm_size()) < 0) {
    AERROR << "ftruncate failed: " << strerror(errno);
    close(fd);
    return false;
  }

  // attach managed_shm_  将打开的文件映射到内存
  managed_shm_ = mmap(nullptr, conf_.managed_shm_size(), PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
  if (managed_shm_ == MAP_FAILED) {
    AERROR << "attach shm failed:" << strerror(errno);
    close(fd);
    // 删除/dev/shm目录的文件,shm_unlink 删除的文件是由shm_open函数创建于/dev/shm目录的
    shm_unlink(shm_name_.c_str());
    return false;
  }

  close(fd);

  // create field state_
  state_ = new (managed_shm_) State(conf_.ceiling_msg_size());
  if (state_ == nullptr) {
    AERROR << "create state failed.";
    //只是将映射的内存从进程的地址空间撤销，如果不调用这个函数，则在进程终止前，该片区域将得不到释放
    munmap(managed_shm_, conf_.managed_shm_size());
    managed_shm_ = nullptr;
    shm_unlink(shm_name_.c_str());
    return false;
  }

  conf_.Update(state_->ceiling_msg_size());

  // create field blocks_
  blocks_ = new (static_cast<char*>(managed_shm_) + sizeof(State))
      Block[conf_.block_num()];
  if (blocks_ == nullptr) {
    AERROR << "create blocks failed.";
    state_->~State();
    state_ = nullptr;
    munmap(managed_shm_, conf_.managed_shm_size());
    managed_shm_ = nullptr;
    shm_unlink(shm_name_.c_str());
    return false;
  }

  // create block buf
  uint32_t i = 0;
  for (; i < conf_.block_num(); ++i) {
    uint8_t* addr =
        new (static_cast<char*>(managed_shm_) + sizeof(State) +
             conf_.block_num() * sizeof(Block) + i * conf_.block_buf_size())
            uint8_t[conf_.block_buf_size()];

    if (addr == nullptr) {
      break;
    }

    std::lock_guard<std::mutex> lg(block_buf_lock_);
    block_buf_addrs_[i] = addr;
  }

  if (i != conf_.block_num()) {
    AERROR << "create block buf failed.";
    state_->~State();
    state_ = nullptr;
    blocks_ = nullptr;
    {
      std::lock_guard<std::mutex> lg(block_buf_lock_);
      block_buf_addrs_.clear();
    }
    munmap(managed_shm_, conf_.managed_shm_size());
    managed_shm_ = nullptr;
    shm_unlink(shm_name_.c_str());
    return false;
  }

  state_->IncreaseReferenceCounts();
  init_ = true;
  return true;
}

bool PosixSegment::OpenOnly() {
  if (init_) {
    return true;
  }

  // get managed_shm_
  int fd = shm_open(shm_name_.c_str(), O_RDWR, 0644);
  if (fd == -1) {
    AERROR << "get shm failed: " << strerror(errno);
    return false;
  }

  struct stat file_attr;
  if (fstat(fd, &file_attr) < 0) {
    AERROR << "fstat failed: " << strerror(errno);
    close(fd);
    return false;
  }

  // attach managed_shm_
  managed_shm_ = mmap(nullptr, file_attr.st_size, PROT_READ | PROT_WRITE,
                      MAP_SHARED, fd, 0);
  if (managed_shm_ == MAP_FAILED) {
    AERROR << "attach shm failed: " << strerror(errno);
    close(fd);
    return false;
  }

  close(fd);
  // get field state_
  state_ = reinterpret_cast<State*>(managed_shm_);
  if (state_ == nullptr) {
    AERROR << "get state failed.";
    munmap(managed_shm_, file_attr.st_size);
    managed_shm_ = nullptr;
    return false;
  }

  conf_.Update(state_->ceiling_msg_size());

  // get field blocks_
  blocks_ = reinterpret_cast<Block*>(static_cast<char*>(managed_shm_) +
                                     sizeof(State));
  if (blocks_ == nullptr) {
    AERROR << "get blocks failed.";
    state_ = nullptr;
    munmap(managed_shm_, conf_.managed_shm_size());
    managed_shm_ = nullptr;
    return false;
  }

  // get block buf
  uint32_t i = 0;
  for (; i < conf_.block_num(); ++i) {
    uint8_t* addr = reinterpret_cast<uint8_t*>(
        static_cast<char*>(managed_shm_) + sizeof(State) +
        conf_.block_num() * sizeof(Block) + i * conf_.block_buf_size());

    std::lock_guard<std::mutex> lg(block_buf_lock_);
    block_buf_addrs_[i] = addr;
  }

  if (i != conf_.block_num()) {
    AERROR << "open only failed.";
    state_->~State();
    state_ = nullptr;
    blocks_ = nullptr;
    {
      std::lock_guard<std::mutex> lg(block_buf_lock_);
      block_buf_addrs_.clear();
    }
    munmap(managed_shm_, conf_.managed_shm_size());
    managed_shm_ = nullptr;
    shm_unlink(shm_name_.c_str());
    return false;
  }

  state_->IncreaseReferenceCounts();
  init_ = true;
  ADEBUG << "open only true.";
  return true;
}

bool PosixSegment::Remove() {
  if (shm_unlink(shm_name_.c_str()) < 0) {
    AERROR << "shm_unlink failed: " << strerror(errno);
    return false;
  }
  return true;
}

void PosixSegment::Reset() {
  state_ = nullptr;
  blocks_ = nullptr;
  {
    std::lock_guard<std::mutex> lg(block_buf_lock_);
    block_buf_addrs_.clear();
  }
  if (managed_shm_ != nullptr) {
    munmap(managed_shm_, conf_.managed_shm_size());
    managed_shm_ = nullptr;
    return;
  }
}

}  // namespace transport
}  // namespace cyber
}  // namespace apollo
