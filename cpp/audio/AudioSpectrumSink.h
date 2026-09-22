// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 QueMusic Contributors
//
// 频谱订阅者接口：由音频回调线程直接调用，实现方需自行加锁
#pragma once

class AudioSpectrumSink
{
public:
    virtual ~AudioSpectrumSink() = default;
    virtual void pushSamples(const float *interleaved, int frames, int channels, int sampleRate) = 0;
};
