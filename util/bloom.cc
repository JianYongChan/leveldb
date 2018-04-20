// Copyright (c) 2012 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/filter_policy.h"

#include "leveldb/slice.h"
#include "util/hash.h"

namespace leveldb {

namespace {
static uint32_t BloomHash(const Slice& key) {
  return Hash(key.data(), key.size(), 0xbc9f1d34);
}
// bloom filter的基本思想是：
// 如果某条过滤信息有n个键，则可以创建一个n*bits_per_key位的位数组
// 然后对每个key进行n此运算，得到n个值，然后在位数组中把对应的n个位(计算出3,5,7则把第3,5,7为置1)设置为1即可
// 当验证时，则对查找的key也是进行n次此运算，得到n个数，在位数组中看看这n个位是否都为1
// 如果全部为1，则说明查找的key在这个过滤器中；否则只要有一个不为1，就说明该key不再filter中
// 这里可以看出，如果bloom filter报告某个key不再该filter中，则它一定时不在的；
// 但是如果bloom filter 报告某个key在filter中，则实际上不一定在。

class BloomFilterPolicy : public FilterPolicy {
 private:
  size_t bits_per_key_;
  size_t k_; // k_实际上时计算时采用的hash function的个数

 public:
  explicit BloomFilterPolicy(int bits_per_key)
      : bits_per_key_(bits_per_key) {
    // We intentionally round down to reduce probing cost a little bit
    k_ = static_cast<size_t>(bits_per_key * 0.69);  // 0.69 =~ ln(2) (为什么不是～=)
    if (k_ < 1) k_ = 1;
    if (k_ > 30) k_ = 30;
  }

  // 不知道返回名字有什么用处
  virtual const char* Name() const {
    return "leveldb.BuiltinBloomFilter2";
  }

  virtual void CreateFilter(const Slice* keys, int n, std::string* dst) const {
    // Compute bloom filter size (in both bits and bytes)
    size_t bits = n * bits_per_key_;

    // For small n, we can see a very high false positive rate.  Fix it
    // by enforcing a minimum bloom filter length.
    if (bits < 64) bits = 64;
    
    // 向上取整
    size_t bytes = (bits + 7) / 8;
    bits = bytes * 8;

    // 是添加了key之后要把bloom filter扩大(装新增的keys)
    // 需要注意的是，std::string中的resize方法的第二个参数只是填充新增加的空间
    // 而不会覆盖原来的空间
    // std::string str = "PengYiXi"; str.resize(str.size() + 3, '7')
    // 得到的str为"PengYiXi777"
    const size_t init_size = dst->size();
    dst->resize(init_size + bytes, 0);
    // 把hash次数(实际上是偏移次数)放入bloom filter尾部
    dst->push_back(static_cast<char>(k_)); // Remember # of probes in filter
    
    // 从新增的keys表示的位数组开始
    char* array = &(*dst)[init_size];
    // leveldb其实并未给出多个独立的hash function，
    // 而是通过一个hash function得到一个原始的hash值
    // 然后再通过原始的hash值进行偏移得到新的hash值
    for (int i = 0; i < n; i++) {
      // Use double-hashing to generate a sequence of hash values.
      // See analysis in [Kirsch,Mitzenmacher 2006].
      uint32_t h = BloomHash(keys[i]);
      // 这条语句的作用是把h的左15位和右17位位置对调
      // 简直了，厉害！
      const uint32_t delta = (h >> 17) | (h << 15);  // Rotate right 17 bits
      // 从这里可以看出k_表示进行偏移的次数
      // 也就相当于hash function的个数
      for (size_t j = 0; j < k_; j++) {
        const uint32_t bitpos = h % bits;
        // 这一条语句也需要好好琢磨一下
        // 一次hash只设置一个bit，然而array里面每个元素都占一个byte
        // bitpos/8得到的是要设置为1的那个bit所在的字节(byte)
        // 1<<(bitpos%8)得到的是字节内所要设置的bit
        // 至于用位或，是因为该字节其余bit位上的旧值需要保留(毕竟只是更新一个bit)
        // 比如hash得到的值是37，表示第4个字节(从0开始)的第4个(从0开始)bit需要为1
        array[bitpos/8] |= (1 << (bitpos % 8));
        h += delta;
      }
    }
  }

  virtual bool KeyMayMatch(const Slice& key, const Slice& bloom_filter) const {
    const size_t len = bloom_filter.size();
    // 因为至少有一个k_再bloom filter里面
    // 所以len<2则说明key一定不在里面
    if (len < 2) return false;

    const char* array = bloom_filter.data();
    // len-1是因为bloom filter里面还存了一个k_
    const size_t bits = (len - 1) * 8;

    // Use the encoded k so that we can read filters generated by
    // bloom filters created using different parameters.
    const size_t k = array[len-1];
    // 不太懂什么意思，为什么k>30就一定match？
    if (k > 30) {
      // Reserved for potentially new encodings for short bloom filters.
      // Consider it a match.
      return true;
    }

    uint32_t h = BloomHash(key);
    const uint32_t delta = (h >> 17) | (h << 15);  // Rotate right 17 bits
    for (size_t j = 0; j < k; j++) {
      const uint32_t bitpos = h % bits;
      // 只要有一位为0，就肯定不在里面
      if ((array[bitpos/8] & (1 << (bitpos % 8))) == 0) return false;
      h += delta;
    }
    return true;
  }
};
}

const FilterPolicy* NewBloomFilterPolicy(int bits_per_key) {
  return new BloomFilterPolicy(bits_per_key);
}

}  // namespace leveldb
