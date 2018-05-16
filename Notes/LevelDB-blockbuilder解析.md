# block结构

## table的结构

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
[shared-len][non_shared-len][value-len][non-shared-key][value]
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

源码文件

## block的构建流程

blockbuilder先将一条一条的entry(|shared-len|non-shared-len|value-len|non-shared-key|value|)添加到buffer，并且以`interval`为重启点之间的间隔，将重启点在当前blockbuilder所在的block中的偏移位置另外保存，当block的大小达到了阈值，就构建完成，此时将所有的重启点以及重启点的个数也添加到buffer中，此时就完成了一个block的构建。

### block builder的结构

``` C++
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
  const Options*        options_;     // table的一些配置选项，比如重启点的间隔(interval)
  std::string           buffer_;      // Destination buffer
  // restart_存储的是当前block的每个重启点在该block中的偏移位置
  std::vector<uint32_t> restarts_;    // Restart points
  // counter_存储的是最新的重起点块存储了多少条entry了
  // 默认每16条entry一个重启点，我将这16个entry称为一个重启点块
  // 重启点块的大小由options_.block_restart_interval决定(默认16)
  int                   counter_;     // Number of entries emitted since restart 从上个重启点开始到现在存了多少个entry了？
  bool                  finished_;    // Has Finish() been called?
  std::string           last_key_;    // 上一条插入的entry的key

  // No copying allowed
  BlockBuilder(const BlockBuilder&);
  void operator=(const BlockBuilder&);
};
```

### block builder添加key到buffer

```C++
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

  // Add "<shared><non_shared><value_size>" to buffer_
  // data block中一条entry的结构是：|shared(varint32)|non-shared(varint32)||value-size(varint32)|non-shared-key|value|
  PutVarint32(&buffer_, shared);
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