// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/filter_block.h"

#include "leveldb/filter_policy.h"
#include "util/coding.h"

namespace leveldb {

// See doc/table_format.md for an explanation of the filter block format.

// filter block存储的是一系列的filter
// 第i个filter存储有FilterPolicy::CreateFilter()在某些key上产生的输出
// 这些key的文件偏移落在[ i*base ... (i+1)*base-1 ]内，默认base为2KB
// 举个栗子：block X和block Y落在[ 0 ... 2KB-1 ]内
// 那么X和Y中的keys都会通过调用FilterPolicy::CreateFilter()来产生一个filter
// 并且这个filter是第一个filter，作为filter block中的首个filter存储
// filter block的结构
// [filter 0]
// [filter 1]
// [filter 2]
// ...
// [filter n]
// [offset-of-filter-0]
// [offset-of-filter-1]
// ..
// [offset-of-filter-n]
// [offset-of-beginning-of-offset-array]
// [baselg]


// Generate new filter every 2KB of data
static const size_t kFilterBaseLg = 11;
static const size_t kFilterBase = 1 << kFilterBaseLg;

FilterBlockBuilder::FilterBlockBuilder(const FilterPolicy* policy)
    : policy_(policy) {
}

// block_offset存储的是什么的offset？
// 我觉得应该是data block的结束点的偏移位置
// 当一个块被刷新到磁盘时，就调用StartBlock函数(在table_builder.cc中可以看到)
void FilterBlockBuilder::StartBlock(uint64_t block_offset) {
  uint64_t filter_index = (block_offset / kFilterBase);
  // filter_index表示当前的keys需要多少个filter
  // filter_offsets_.size()返回的是当前filter的个数
  // 因为块被刷新到了磁盘，所以filter_index一定要大于filter_offsets_.size()
  assert(filter_index >= filter_offsets_.size());
  while (filter_index > filter_offsets_.size()) {
    GenerateFilter();
  }
}

void FilterBlockBuilder::AddKey(const Slice& key) {
  Slice k = key;
  // start_里面存储是key的偏移位置
  start_.push_back(keys_.size());
  keys_.append(k.data(), k.size());
}

Slice FilterBlockBuilder::Finish() {
  if (!start_.empty()) {
    GenerateFilter();
  }

  // Append array of per-filter offsets
  // 从这里可以看出result_的结构：
  // [filter 1]
  // [filter 2]
  // ...
  // [filter n]
  // [len-of-filter 1]
  // [len-of-filter 2]
  // ...
  // [len-of-filter n]
  // [number-of-filter]
  // [kFilterBaseLg]
  const uint32_t array_offset = result_.size();
  for (size_t i = 0; i < filter_offsets_.size(); i++) {
    PutFixed32(&result_, filter_offsets_[i]);
  }

  PutFixed32(&result_, array_offset);
  result_.push_back(kFilterBaseLg);  // Save encoding parameter in result
  return Slice(result_);
}

// 联系GenerateFilter被调用的地方
// 在StartBlock函数中，，
void FilterBlockBuilder::GenerateFilter() {
  const size_t num_keys = start_.size();
  if (num_keys == 0) {
    // Fast path if there are no keys for this filter
    filter_offsets_.push_back(result_.size());
    return;
  }

  // Make list of keys from flattened key structure
  start_.push_back(keys_.size());  // Simplify length computation
  tmp_keys_.resize(num_keys);
  // 将所有的keys都存放到tmp_keys_里面
  // 用做CreateFilter的参数
  for (size_t i = 0; i < num_keys; i++) {
    const char* base = keys_.data() + start_[i];
    // 这里可以看到每一条key是通过相邻key的偏移值相减得到的
    // 所以上面`start_push_back(keys_.size())是有必要的
    // 因为这样才能得到最后一条key的length
    size_t length = start_[i+1] - start_[i];
    tmp_keys_[i] = Slice(base, length);
  }

  // Generate filter for current set of keys and append to result_.
  filter_offsets_.push_back(result_.size());
  // 因为vector是占用连续的内存
  // 所以&tmp_keys[0]获得keys的起始位置
  policy_->CreateFilter(&tmp_keys_[0], static_cast<int>(num_keys), &result_);

  // 重置这3个成员变量
  tmp_keys_.clear();
  keys_.clear();
  start_.clear();
}

FilterBlockReader::FilterBlockReader(const FilterPolicy* policy,
                                     const Slice& contents)
   : policy_(policy),
      data_(NULL),
      offset_(NULL),
      num_(0),
      base_lg_(0) {
  size_t n = contents.size();
  if (n < 5) return;  // 1 byte for base_lg_ and 4 for start of offset array
  base_lg_ = contents[n-1];
  // last_word是
  uint32_t last_word = DecodeFixed32(contents.data() + n - 5);
  if (last_word > n - 5) return;
  data_ = contents.data();
  offset_ = data_ + last_word;
  num_ = (n - 5 - last_word) / 4;
}

bool FilterBlockReader::KeyMayMatch(uint64_t block_offset, const Slice& key) {
  uint64_t index = block_offset >> base_lg_;
  if (index < num_) {
    uint32_t start = DecodeFixed32(offset_ + index*4);
    uint32_t limit = DecodeFixed32(offset_ + index*4 + 4);
    if (start <= limit && limit <= static_cast<size_t>(offset_ - data_)) {
      Slice filter = Slice(data_ + start, limit - start);
      return policy_->KeyMayMatch(key, filter);
    } else if (start == limit) {
      // Empty filters do not match any keys
      return false;
    }
  }
  return true;  // Errors are treated as potential matches
}

}
