// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/table_builder.h"

#include <assert.h>
#include "leveldb/comparator.h"
#include "leveldb/env.h"
#include "leveldb/filter_policy.h"
#include "leveldb/options.h"
#include "table/block_builder.h"
#include "table/filter_block.h"
#include "table/format.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace leveldb {

struct TableBuilder::Rep {
  Options options;
  Options index_block_options;
  WritableFile* file;
  // 记录table的偏移量，主要用于index block
  uint64_t offset;
  // 每次操作的状态
  Status status;
  // 存放data block
  BlockBuilder data_block;
  // 存放index block
  BlockBuilder index_block;
  std::string last_key;
  // table中的key-value对个数
  int64_t num_entries;
  bool closed;          // Either Finish() or Abandon() has been called.
  FilterBlockBuilder* filter_block;

  // We do not emit the index entry for a block until we have seen the
  // first key for the next data block.  This allows us to use shorter
  // keys in the index block.  For example, consider a block boundary
  // between the keys "the quick brown fox" and "the who".  We can use
  // "the r" as the key for the index block entry since it is >= all
  // entries in the first block and < all entries in subsequent
  // blocks.
  //
  // Invariant: r->pending_index_entry is true only if data_block is empty.

  // index block中存放的也是key-value对
  // key: 所指向的data block的最后(大)的key，value: 指向该data block的BlockHandle(offset, size)
  // 直到下一个data block的第一个key出现我们才为当前block写入一条entry到index block
  // 比如说如果当前data block的最大key为"the quick brown fox"，下一个data block的最下key为"the who"
  // 那么我们为当前data block存的index entry的key就可以为"the r"
  // 因为它比当前data block的所有key都大，而且比下一个data block的所有key都小
  bool pending_index_entry;
  BlockHandle pending_handle;  // Handle to add to index block

  std::string compressed_output; // 压缩后的结果

  Rep(const Options& opt, WritableFile* f)
      : options(opt),
        index_block_options(opt),
        file(f),
        offset(0),
        data_block(&options),
        index_block(&index_block_options),
        num_entries(0),
        closed(false),
        filter_block(opt.filter_policy == NULL ? NULL
                     : new FilterBlockBuilder(opt.filter_policy)),
        pending_index_entry(false) {
    index_block_options.block_restart_interval = 1;
  }
};

TableBuilder::TableBuilder(const Options& options, WritableFile* file)
    : rep_(new Rep(options, file)) {
  if (rep_->filter_block != NULL) {
    rep_->filter_block->StartBlock(0);
  }
}

TableBuilder::~TableBuilder() {
  assert(rep_->closed);  // Catch errors where caller forgot to call Finish()
  // 这里我有点疑问，filter_block这个变量虽然是堆上的，的确需要delete(如果不为NULL)
  // 但是它是在rep_这个变量里面啊
  // delete rep_时不会同时删除filter_block么？
  // 这不会造成二次删除么？
  delete rep_->filter_block; // 不用检查filter_block是否为NULL么？
  delete rep_;
}

Status TableBuilder::ChangeOptions(const Options& options) {
  // Note: if more fields are added to Options, update
  // this function to catch changes that should not be allowed to
  // change in the middle of building a Table.
  if (options.comparator != rep_->options.comparator) {
    return Status::InvalidArgument("changing comparator while building table");
  }

  // Note that any live BlockBuilders point to rep_->options and therefore
  // will automatically pick up the updated options.
  rep_->options = options;
  rep_->index_block_options = options;
  rep_->index_block_options.block_restart_interval = 1;
  return Status::OK();
}

void TableBuilder::Add(const Slice& key, const Slice& value) {
  Rep* r = rep_;
  assert(!r->closed);
  if (!ok()) return;
  if (r->num_entries > 0) {
    // 由于key是有序的，所以新增的key必须要和上一条(last_key)必须要满足Compare关系
    assert(r->options.comparator->Compare(key, Slice(r->last_key)) > 0);
  }

  // 如果Add的key是当前data_block的第一个key
  // 当且仅当data block为空时，pending_index_entry才为true
  // 而且因为只有当新开辟的data block的第一个key出现了，才会为前一个data block写入一条entry到index block中去
  // 这里的”新开辟“和”前一个“不太好理解
  // 比如data-block[2]开辟了，正准备添加第一个key，此时才可以把data-block[1]对应的entry写入到index block中去
  if (r->pending_index_entry) {
    // 只有当前data_block为empty才说明现时Add的key是data_block的第一个key
    assert(r->data_block.empty());
    // 找到现在这个key和上一条key(last_key)之间的一个分隔字符串
    // 比如last_key为abcd，现时的key为abcz，那么得到的分隔字符床为abce，即last_key与现时的key第一个不同的字符+1
    r->options.comparator->FindShortestSeparator(&r->last_key, key);
    std::string handle_encoding;
    // 将e上一个data block的offset, size写入到String中以便后面作为entry写入到index block中
    r->pending_handle.EncodeTo(&handle_encoding);
    // 现在添加的key是新data_block的第一个key，所以就要把上一个block的index entry加入到index_block内
    r->index_block.Add(r->last_key, Slice(handle_encoding));
    // 此后的key就不会被添加到index_block中去
    r->pending_index_entry = false;
  }

  if (r->filter_block != NULL) {
    r->filter_block->AddKey(key);
  }

  // 更新last_key，以备下次Add key用
  r->last_key.assign(key.data(), key.size());
  r->num_entries++;
  r->data_block.Add(key, value);

  // 如果当前data_block的大小到达阈值，就将其刷新到磁盘
  const size_t estimated_block_size = r->data_block.CurrentSizeEstimate();
  if (estimated_block_size >= r->options.block_size) {
    Flush();
  }
}

void TableBuilder::Flush() {
  Rep* r = rep_;
  assert(!r->closed);
  if (!ok()) return;
  if (r->data_block.empty()) return;
  assert(!r->pending_index_entry);
  // 将data_block的offset，size写入到pending_handle以供后面为该block创建index entry
  WriteBlock(&r->data_block, &r->pending_handle);
  if (ok()) {
    // 为下一次做准备
    r->pending_index_entry = true;
    // 将data block的数据写入到文件
    r->status = r->file->Flush();
  }
  // 注意到只有Flush的时候才会开辟新的filter block
  if (r->filter_block != NULL) {
    r->filter_block->StartBlock(r->offset);
  }
}

// SSTable文件格式：
// | Data | Compression type | CRC |
// | Data | Compression type | CRC |
// | Data | Compression type | CRC |

// 如果需要压缩，则进行压缩
// 实际的写入工作由WriteRawBlock完成
void TableBuilder::WriteBlock(BlockBuilder* block, BlockHandle* handle) {
  // File format contains a sequence of blocks where each block has:
  //    block_data: uint8[n]
  //    type: uint8 (compression type)
  //    crc: uint32
  assert(ok());
  Rep* r = rep_;
  Slice raw = block->Finish();

  Slice block_contents;
  CompressionType type = r->options.compression;
  // TODO(postrelease): Support more compression options: zlib?
  switch (type) { // 不进行压缩
    case kNoCompression:
      block_contents = raw;
      break;

    case kSnappyCompression: { // snappy压缩(尚不了解)
      std::string* compressed = &r->compressed_output;
      if (port::Snappy_Compress(raw.data(), raw.size(), compressed) &&
          compressed->size() < raw.size() - (raw.size() / 8u)) {
        block_contents = *compressed;
      } else { // 压缩率小于12.5%，就不压缩而直接存储
        // Snappy not supported, or compressed less than 12.5%, so just
        // store uncompressed form
        block_contents = raw;
        type = kNoCompression;
      }
      break;
    }
  }
  WriteRawBlock(block_contents, type, handle);
  r->compressed_output.clear();
  block->Reset();
}

// WriteRawBlock用于将数据写入到block中去
void TableBuilder::WriteRawBlock(const Slice& block_contents,
                                 CompressionType type,
                                 BlockHandle* handle) {
  Rep* r = rep_;
  handle->set_offset(r->offset);
  handle->set_size(block_contents.size());
  r->status = r->file->Append(block_contents);
  if (r->status.ok()) {
    // 1bit的压缩类型和4bit的CRC校验码
    char trailer[kBlockTrailerSize];
    trailer[0] = type;
    uint32_t crc = crc32c::Value(block_contents.data(), block_contents.size());
    crc = crc32c::Extend(crc, trailer, 1);  // Extend crc to cover block type
    EncodeFixed32(trailer+1, crc32c::Mask(crc));
    r->status = r->file->Append(Slice(trailer, kBlockTrailerSize));
    if (r->status.ok()) {
      r->offset += block_contents.size() + kBlockTrailerSize;
    }
  }
}

Status TableBuilder::status() const {
  return rep_->status;
}

Status TableBuilder::Finish() {
  Rep* r = rep_;
  Flush();
  assert(!r->closed);
  r->closed = true;

  // handle存储的其实都是offset, size两个变量
  BlockHandle filter_block_handle, metaindex_block_handle, index_block_handle;

  // Write filter block
  if (ok() && r->filter_block != NULL) {
    WriteRawBlock(r->filter_block->Finish(), kNoCompression,
                  &filter_block_handle);
  }

  // Write metaindex block
  // 可以看到mataindex存储的其实就是一条entry
  // key为filter的名字(还有前缀filter.)，value就是指向filter block的BockHandle
  if (ok()) {
    BlockBuilder meta_index_block(&r->options);
    if (r->filter_block != NULL) {
      // Add mapping from "filter.Name" to location of filter data
      std::string key = "filter.";
      key.append(r->options.filter_policy->Name());
      std::string handle_encoding;
      filter_block_handle.EncodeTo(&handle_encoding);
      meta_index_block.Add(key, handle_encoding);
    }

    // TODO(postrelease): Add stats and other meta blocks
    // 当下meta block中只有filter, 而没有存储其他元数据
    WriteBlock(&meta_index_block, &metaindex_block_handle);
  }

  // Write index block
  if (ok()) {
    // 如果当前的pengding_index_entry为true，说明上一个data block已经结束
    // 但是生成index entry的规则是：下一个data block添加第一个key
    // 所以有可能这个data block刚刚结束，下一个data block尚未开始
    // 所以这个data block的index entry就没有被添加到r->index_block中去
    if (r->pending_index_entry) {
      r->options.comparator->FindShortSuccessor(&r->last_key);
      std::string handle_encoding;
      r->pending_handle.EncodeTo(&handle_encoding);
      r->index_block.Add(r->last_key, Slice(handle_encoding));
      r->pending_index_entry = false;
    }
    WriteBlock(&r->index_block, &index_block_handle);
  }

  // Write footer
  if (ok()) {
    Footer footer;
    footer.set_metaindex_handle(metaindex_block_handle);
    footer.set_index_handle(index_block_handle);
    std::string footer_encoding;
    footer.EncodeTo(&footer_encoding);
    r->status = r->file->Append(footer_encoding);
    if (r->status.ok()) {
      r->offset += footer_encoding.size();
    }
  }
  return r->status;
}

void TableBuilder::Abandon() {
  Rep* r = rep_;
  assert(!r->closed);
  r->closed = true;
}

uint64_t TableBuilder::NumEntries() const {
  return rep_->num_entries;
}

uint64_t TableBuilder::FileSize() const {
  return rep_->offset;
}

}  // namespace leveldb
