#pragma once

#include <vector>
#include <cstring>
#include <algorithm>

namespace chwell {
namespace core {

/**
 * @brief 环形缓冲区：读写只移动索引，零拷贝
 *
 * 替代 std::vector<char> + head_ 偏移量方案，
 * 消除 compact_prefix() 中的 O(N) 内存搬移。
 *
 * 性能特征：
 * - write/peek/consume: O(1)，只移动索引
 * - 扩容时一次性线性化，均摊 O(1)
 * - 跨越尾部时分两段操作，最多 2 次 memcpy
 */
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity = 4096)
        : buffer_(std::max(capacity, size_t(64))),  // 最小 64 字节
          read_pos_(0),
          write_pos_(0),
          size_(0) {}

    /**
     * @brief 写入数据到缓冲区
     * 如果容量不足会自动扩容（2 倍增长）
     */
    void write(const char* data, size_t len) {
        if (len == 0) return;
        ensure_capacity(size_ + len);

        // 可能跨越尾部，分两段写
        size_t first = std::min(len, buffer_.size() - write_pos_);
        std::memcpy(buffer_.data() + write_pos_, data, first);
        if (first < len) {
            std::memcpy(buffer_.data(), data + first, len - first);
        }
        write_pos_ = (write_pos_ + len) % buffer_.size();
        size_ += len;
    }

    /**
     * @brief 读取数据（不消费，只拷贝到 out）
     * @param out 输出缓冲区
     * @param len 要读取的字节数
     * @return 实际读取的字节数
     */
    size_t peek(char* out, size_t len) const {
        len = std::min(len, size_);
        if (len == 0) return 0;

        size_t first = std::min(len, buffer_.size() - read_pos_);
        std::memcpy(out, buffer_.data() + read_pos_, first);
        if (first < len) {
            std::memcpy(out + first, buffer_.data(), len - first);
        }
        return len;
    }

    /**
     * @brief 消费数据（移动读指针，零拷贝）
     */
    void consume(size_t len) {
        len = std::min(len, size_);
        if (len == 0) return;
        read_pos_ = (read_pos_ + len) % buffer_.size();
        size_ -= len;
    }

    /**
     * @brief 读取并消费数据（peek + consume 的便捷组合）
     */
    size_t read(char* out, size_t len) {
        size_t n = peek(out, len);
        consume(n);
        return n;
    }

    // 可读字节数
    size_t readable() const { return size_; }

    // 是否为空
    bool empty() const { return size_ == 0; }

    // 清空缓冲区
    void clear() { read_pos_ = write_pos_ = size_ = 0; }

    // 总容量
    size_t capacity() const { return buffer_.size(); }

    // 可写容量
    size_t writable() const { return buffer_.size() - size_; }

    /**
     * @brief 预留容量（如果当前容量不足则扩容）
     */
    void reserve(size_t needed) {
        if (needed > buffer_.size()) {
            ensure_capacity(needed);
        }
    }

private:
    void ensure_capacity(size_t needed) {
        if (needed <= buffer_.size()) return;

        // 2 倍增长
        size_t new_cap = buffer_.size();
        while (new_cap < needed) new_cap *= 2;

        // 将数据线性化到新缓冲区
        std::vector<char> new_buf(new_cap);
        if (size_ > 0) {
            size_t first = std::min(size_, buffer_.size() - read_pos_);
            std::memcpy(new_buf.data(), buffer_.data() + read_pos_, first);
            if (first < size_) {
                std::memcpy(new_buf.data() + first, buffer_.data(), size_ - first);
            }
        }
        buffer_ = std::move(new_buf);
        read_pos_ = 0;
        write_pos_ = size_;
    }

    std::vector<char> buffer_;
    size_t read_pos_;
    size_t write_pos_;
    size_t size_;
};

} // namespace core
} // namespace chwell
