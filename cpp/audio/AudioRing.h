// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 QueMusic Contributors
//
// 单生产者/单消费者无锁环形缓冲：解码线程写，音频回调线程读
#pragma once

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

class AudioRing
{
public:
    void configure(int capacityFrames, int channels)
    {
        m_channels = std::max(1, channels);
        m_capacity = std::max(1024, capacityFrames) * m_channels;
        m_data.assign(size_t(m_capacity), 0.0f);
        m_head.store(0, std::memory_order_relaxed);
        m_tail.store(0, std::memory_order_relaxed);
    }

    int channels() const { return m_channels; }
    int capacity() const { return m_capacity; }

    int available() const
    {
        return m_tail.load(std::memory_order_acquire) - m_head.load(std::memory_order_acquire);
    }

    int space() const { return m_capacity - available(); }

    void clear()
    {
        m_head.store(0, std::memory_order_relaxed);
        m_tail.store(0, std::memory_order_relaxed);
    }

    int write(const float *src, int count)
    {
        const int h = m_head.load(std::memory_order_relaxed);
        const int t = m_tail.load(std::memory_order_relaxed);
        const int n = std::min(count, m_capacity - (t - h));
        if (n <= 0)
            return 0;
        const int start = t % m_capacity;
        const int first = std::min(n, m_capacity - start);
        std::memcpy(m_data.data() + start, src, size_t(first) * sizeof(float));
        if (n > first)
            std::memcpy(m_data.data(), src + first, size_t(n - first) * sizeof(float));
        m_tail.store(t + n, std::memory_order_release);
        return n;
    }

    int read(float *dst, int count)
    {
        const int h = m_head.load(std::memory_order_relaxed);
        const int t = m_tail.load(std::memory_order_acquire);
        const int n = std::min(count, t - h);
        if (n <= 0)
            return 0;
        const int start = h % m_capacity;
        const int first = std::min(n, m_capacity - start);
        std::memcpy(dst, m_data.data() + start, size_t(first) * sizeof(float));
        if (n > first)
            std::memcpy(dst + first, m_data.data(), size_t(n - first) * sizeof(float));
        m_head.store(h + n, std::memory_order_release);
        return n;
    }

private:
    std::vector<float> m_data;
    std::atomic<int> m_head{0};
    std::atomic<int> m_tail{0};
    int m_capacity = 0;
    int m_channels = 2;
};
