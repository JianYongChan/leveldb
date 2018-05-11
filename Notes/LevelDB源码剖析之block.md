# table的结构

SST文件是LevelDB最后的存储角色，划分为不同的level。LevelDB将SST文件定义为table，每个table又划分为多个连续的block，每个block都存储有多条entry。

```C++
[beginning_of_file]
[data block 1]
[data block 2]
...
[data block N]
[meta block 1]
[meta block 2]
...
[meta block M]
[metaindex block]
[index block]
[footer]
[end_of_file]
```

可以看到一个table里面有很多个也有很多中block，但是现在不需要了解其他的block是干啥用的，只需要知道data block是用来存储key-value对(entry)的就行了。而且data block的entry有一个特点：

* 每一条entry都是根据其key的顺序来进行存储的

这样，由于每一条entry都是根据key的顺序来进行存储，那么key就会有相同的部分，那么可以将key进行压缩存储: 第一条entry的key全部存储，后面的key只存储与之不同的部分即可，但是这样有缺点:

1. 如果开头的数据损坏，那么其后所有的entry都无法恢复

2. 如果要取出第100条entry，那么还需要把前99条entry同时取出。这样效率太低。

所以，LevelDB规定了每多少条entry进行一次整个key的存储（存储有整个key的entry称为重启点)，entry的条数由`options_.block_restart_interval`(默认是16，在options.cc中可以看到)决定，也就是说默认每16条entry就会有一个重启点。那么为了知道第n条记录和第n-1条记录的key有多少是相同(共享的)，还需要存储key相同部分的长度，key不同部分的长度，entry中value的长度，所以一条entry的结构是这样的：

```C++
[shared][non_shared][value_len][key][value]
```

而整个data block是这样的：

```C++
[entry 0]
[entry 1]
...
[entry 15] // 重启点
[entry 16]
...
[entry N-1]
[restart point 0] // 重启点数组
[restart point 1]
...
[restart point (N+15) / 16]
[restart point number] // 重启点的个数
```

```C++
// Block的实现
// 可以看出Block其实就是一个char*
//
//
//
//
//
class Block {
 public:
  // Initialize the block with the specified contents.
  explicit Block(const BlockContents& contents);

  ~Block();

  size_t size() const { return size_; }
  Iterator* NewIterator(const Comparator* comparator);

 private:
  uint32_t NumRestarts() const; // 返回重启点的个数

  // 一个block其实就是一个字符串
  const char* data_;
  size_t size_;

  // 记录重启点数组在block这个长字符串中的偏移量
  uint32_t restart_offset_;     // Offset in data_ of restart array
  // data_是否为堆分配的内存，是的话就需要手动回收
  bool owned_;                  // Block owns data_[]

  // No copying allowed
  Block(const Block&);
  void operator=(const Block&);

  class Iter;                   // 内部迭代器，用于迭代key-value对
};
```

## block所需函数的整体布局

理解block需要看这些文件：

```C++
block_build.h    //
block_build.cc
block.h          //
block.cc
```

### block中的一些函数分析

```C++
// 创建一个block
// 在创建block之前需要对builder进行Reset
// 然后才可以进行Add动作
// block创建好了之后builder需要进行Finish
//
class BlockBuilder {
 public:
  explicit BlockBuilder(const Options* options);

  // Reset the contents as if the BlockBuilder was just constructed.
  void Reset();

  // REQUIRES: Finish() has not been called since the last call to Reset().
  // REQUIRES: key is larger than any previously added key
  void Add(const Slice& key, const Slice& value);

  // Finish building the block and return a slice that refers to the
  // block contents.  The returned slice will remain valid for the
  // lifetime of this builder or until Reset() is called.
  Slice Finish();

  // Returns an estimate of the current (uncompressed) size of the block
  // we are building.
  size_t CurrentSizeEstimate() const;

  // Return true iff no entries have been added since the last Reset()
  bool empty() const {
    return buffer_.empty();
  }

 private:
  const Options*        options_;     // 选项字段，比如interval就由options决定
  std::string           buffer_;      // Destination buffer
  std::vector<uint32_t> restarts_;    // Restart points
  // counter_存储的是最新的重起点块存储了多少条entry了
  // 默认每16条entry一个重启点，我将这16个entry称为一个重启点块
  // 重启点块的大小由options_.block_restart_interval决定(默认16)
  int                   counter_;     // Number of entries emitted since restart
  bool                  finished_;    // Has Finish() been called?
  std::string           last_key_;    // 上一条插入的entry的key

  // No copying allowed
  BlockBuilder(const BlockBuilder&);
  void operator=(const BlockBuilder&);
};
```

```C++
// 添加entry
//
//
//
void BlockBuilder::Add(const Slice& key, const Slice& value) {
  Slice last_key_piece(last_key_);
  assert(!finished_);
  assert(counter_ <= options_->block_restart_interval);
  assert(buffer_.empty() // No values yet?
         || options_->comparator->Compare(key, last_key_piece) > 0);
  size_t shared = 0;
  if (counter_ < options_->block_restart_interval) { // 每block_restart_interval(默认16)条entry一个重启点，如果还没有到16，就继续用这个重启点
    // See how much sharing to do with previous string
    const size_t min_length = std::min(last_key_piece.size(), key.size());
    while ((shared < min_length) && (last_key_piece[shared] == key[shared])) {
      shared++;
    }
  } else { // 设置新重启点
    // Restart compression
    restarts_.push_back(buffer_.size());
    counter_ = 0;
  }
  const size_t non_shared = key.size() - shared;

  // Add "<shared><non_shared><value_size>" to buffer_  PutVarint32(&buffer_, shared);
  PutVarint32(&buffer_, non_shared);
  PutVarint32(&buffer_, value.size());

  // Add string delta to buffer_ followed by value
  buffer_.append(key.data() + shared, non_shared);
  buffer_.append(value.data(), value.size());

  // Update state
  last_key_.resize(shared);
  last_key_.append(key.data() + shared, non_shared);
  assert(Slice(last_key_) == key);
  counter_++;
}
```

```C++
// 解析一条entry
tatic inline const char* DecodeEntry(const char* p, const char* limit,
                                      uint32_t* shared,
                                      uint32_t* non_shared,
                                      uint32_t* value_length) {
  if (limit - p < 3) return NULL; // 如果连3个byte都没有，说明一定出错
  // 为什么需要reinterpret_cast<const unsigned char*>?
  // 我觉得是因为char是带符号，而uint32_t是无符号，所以需要先将带符号转为无符号(想法尚未验证)

  // 这里有一点巧妙
  // 作者首先假设3个数字都是1个byte的情况(有可能是因为这种情况比较常见，所以特殊快速处理)
  // 以1个byte的情况将其取出分别存放到*shared, *non_shared, *value_length
  *shared = reinterpret_cast<const unsigned char*>(p)[0];
  *non_shared = reinterpret_cast<const unsigned char*>(p)[1];
  *value_length = reinterpret_cast<const unsigned char*>(p)[2];
  // 如果满足(*shared | *non_shared | *value_length < 128)则说明3个数的最高位都是0
  // 这就说明这3个数的确都只占1个byte
  // 这种情况就被快速处理了
  if ((*shared | *non_shared | *value_length) < 128) {
    // Fast path: all three values are encoded in one byte each
    p += 3;
  } else {
    // 如果不是快速情况(即3个数每个只占1个byte了)，就在GetVarint32Ptr函数重新解析出这3个数
    if ((p = GetVarint32PtR(p, limit, shared)) == NULL) return NULL;
    if ((p = GetVarint32Ptr(p, limit, non_shared)) == NULL) return NULL;
    if ((p = GetVarint32Ptr(p, limit, value_length)) == NULL) return NULL;
  }

  // limit存储的是什么？
  if (static_cast<uint32_t>(limit - p) < (*non_shared + *value_length)) {
    return NULL;
  }
  return p;
}
```

### block中的迭代器分析

```C++
// 迭代器的定义
class Block::Iter : public Iterator {
 private:
  const Comparator* const comparator_;
  const char* const data_;      // underlying block contents
  uint32_t const restarts_;     // Offset of restart array (list of fixed32)
  uint32_t const num_restarts_; // Number of uint32_t entries in restart array

  // current_ is offset in data_ of current entry.  >= restarts_ if !Valid
  uint32_t current_; // 当前entry在data_中的偏移
  uint32_t restart_index_;  // Index of restart block in which current_ falls
  std::string key_;
  Slice value_;
  Status status_;
};
```

```C++
// 迭代器后退到前一个entry
  virtual void Prev() {
    assert(Valid());

    // Scan backwards to a restart point before current_
    const uint32_t original = current_;
    // 为什么使用while循环
    // 照理来说到前一条entry最多只需到前一个重启点
    // 所以只需要一个if就够了阿
    /*
    if (GetRestartPoint(restart_index_) >= original) {
      if (restart_index == 0){
        current_= restarts_;
        restart_index = num_restarts_;
        return 0;
      }
      restart_index_ --;
    }
    */
    while (GetRestartPoint(restart_index_) >= original) {
      // 如果到达第1个重启点(下标为0), 该重启点对应的重启位置还是在当前entry的位置后面
      // 就将current_设为重启点数组在block中的偏移，将restart_index_设为重启点的数目
      // 这样这个迭代器就是invalid的了(Valid: current_ < restart_)
      if (restart_index_ == 0) { 
        // No more entries
        current_ = restarts_;
        restart_index_ = num_restarts_;
        return;
      }
      restart_index_--;
    }

    SeekToRestartPoint(restart_index_);
    do {
      // Loop until end of current entry hits the start of original entry
      // 这里while循环如果ParseNextKey成功，则还会继续执行NextEntryOffset
      // 说明是先前进到下一条entry再在该entry的基础上检验其下一条entry是否在目的entry之前
    } while (ParseNextKey() && NextEntryOffset() < original);
  }
```

```C++
// 解析下一条entry
bool ParseNextKey() {
    current_ = NextEntryOffset();
    // p指向next entry
    // limit指向重启点数组
    const char* p = data_ + current_;
    const char* limit = data_ + restarts_;  // Restarts come right after data
    if (p >= limit) {
      // No more entries to return.  Mark as invalid.
      current_ = restarts_;
      restart_index_ = num_restarts_;
      return false;
    }

    // Decode next entry
    uint32_t shared, non_shared, value_length;
    p = DecodeEntry(p, limit, &shared, &non_shared, &value_length);
    // 如果p为NULL表示解析失败
    // 由于key的共享长度是指和上一条entry的key相同部分的长度
    // key_.size() < shared即当前entry的key的长度小于下一个entry的key的共享长度
    // 这样就说明next entry不符合要求
    if (p == NULL || key_.size() < shared) {
      CorruptionError();
      return false;
    } else {
      // 更新key
      key_.resize(shared);
      key_.append(p, non_shared);
      // DecodeEntry返回的是指向key不同部分
      // 更新value时需要p+non_shared
      value_ = Slice(p + non_shared, value_length);
      // 为什么需要一个while循环
      // 而不直接一个if语句将restart_index_递增？
      // 因为我觉得迭代器一次前进(Next)最多跳到下一个restart_index_管理的位置
      // 而且用了while循环之后还要使用GetRestartPoint(restart_index_ + 1) < current_这个条件来保证不会跳到正确的重启点下标后面去
      while (restart_index_ + 1 < num_restarts_ &&
             GetRestartPoint(restart_index_ + 1) < current_) {
        ++restart_index_;
      }
      return true;
    }
  }
```

## 网络链接

[data block](https://antonyxux.github.io/2017/05/30/leveldb-1/)

[sstable](https://www.jishux.com/plus/view-733229-1.html)

[leveldb 数据存储](https://www.jianshu.com/p/c7a8eda6eee4)

[leveldb 数据存储](http://catkang.github.io/2017/01/17/leveldb-data.html)

[leveldb block](http://www.voidcn.com/article/p-xatbbwjj-bro.html)

[leveldb sstable block](http://www.cnblogs.com/KevinT/p/3816794.html)

[data block读取分析](http://imzy.me/2017/06/11/leveldb-data-block-%E8%AF%BB%E5%8F%96%E8%A7%A3%E6%9E%90/)