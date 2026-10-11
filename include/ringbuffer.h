#pragma once

#include <stdint.h>
#include <stddef.h>

template <size_t CAPACITY>
class RingBuffer {
public:
    RingBuffer() : head_(0), tail_(0), count_(0) {}

    void clear() {
        head_ = 0;
        tail_ = 0;
        count_ = 0;
    }

    size_t available() const {
        return count_;
    }

    size_t space() const {
        return CAPACITY - count_;
    }

    bool is_full() const {
        return count_ >= CAPACITY;
    }

    bool is_empty() const {
        return count_ == 0;
    }

    bool push(uint8_t byte) {
        if (is_full()) {
            return false;
        }
        buffer_[head_] = byte;
        head_ = (head_ + 1) % CAPACITY;
        count_++;
        return true;
    }

    size_t write(const uint8_t *data, size_t length) {
        size_t written = 0;
        while (written < length && !is_full()) {
            buffer_[head_] = data[written++];
            head_ = (head_ + 1) % CAPACITY;
            count_++;
        }
        return written;
    }

    int pop() {
        if (is_empty()) {
            return -1;
        }
        uint8_t byte = buffer_[tail_];
        tail_ = (tail_ + 1) % CAPACITY;
        count_--;
        return byte;
    }

    size_t read(uint8_t *dest, size_t max_len) {
        size_t read_bytes = 0;
        while (read_bytes < max_len && !is_empty()) {
            dest[read_bytes++] = buffer_[tail_];
            tail_ = (tail_ + 1) % CAPACITY;
            count_--;
        }
        return read_bytes;
    }

    int peek() const {
        if (is_empty()) {
            return -1;
        }
        return buffer_[tail_];
    }

private:
    uint8_t buffer_[CAPACITY];
    volatile size_t head_;
    volatile size_t tail_;
    volatile size_t count_;
};

