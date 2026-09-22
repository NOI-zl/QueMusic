// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 QueMusic Contributors
//
// 变调不变速：先按 pitch 做时长伸缩，再按同倍率重采样把时长还原
// 设源为 S、目标时长倍率 1/speed、目标音高倍率 pitch：
//   伸缩速度 = speed / pitch（时长 × pitch/speed）
//   重采样读指针步进 = pitch（时长 × 1/pitch，音高 × pitch）
// 合成后：时长 × 1/speed，音高 × pitch
#pragma once

#include "TimeStretch.h"

#include <vector>

class PitchShifter
{
public:
    void prepare(int channels, int sampleRate);
    void reset();
    // speed：播放倍速；pitch：音高倍率（1.0 = 不变）
    void setRatios(double speed, double pitch);
    double pitch() const { return m_pitch; }
    double speed() const { return m_speed; }
    bool bypass() const { return m_pitch == 1.0 && m_speed == 1.0; }
    int guardFrames() const { return m_stretch.guardFrames(); }

    void push(const float *in, int frames);
    int pull(float *out, int frames);

private:
    TimeStretch m_stretch;
    int m_channels = 2;
    double m_speed = 1.0;
    double m_pitch = 1.0;
    std::vector<float> m_buf;
    int m_offset = 0;
    double m_pos = 0.0;
};
