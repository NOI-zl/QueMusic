// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 QueMusic Contributors
//
// 音频处理链：10 段参数均衡 + 前级/余量 + 声道工具 + ReplayGain + 峰值限幅
#pragma once

#include <QList>
#include <QString>
#include <QStringList>

#include <array>

class AudioDsp
{
public:
    static constexpr int kBandCount = 10;
    static constexpr int kMaxChannels = 2;

    AudioDsp();

    static const double *bandFrequencies();
    static QStringList bandLabels();
    static QStringList presetNames();
    static QList<qreal> presetGains(const QString &name);

    void prepare(double sampleRate);
    void reset();

    // 旁路时跳过均衡/声道/限幅，但音量与淡变始终生效
    void setBypass(bool on) { m_bypass = on; }
    bool bypassed() const { return m_bypass; }

    void setEqEnabled(bool on);
    bool eqEnabled() const { return m_eqEnabled; }

    void setEqGains(const QList<qreal> &gainsDb);
    // 音频线程专用：直接读数组，不构造 QList（回调内禁止分配）
    void setEqGains(const double *gainsDb, int count);
    QList<qreal> eqGains() const;

    void setEqQ(double q);
    double eqQ() const { return m_eqQ; }

    void setPreampDb(double db);
    double preampDb() const { return m_preampDb; }

    void setAutoHeadroom(bool on);
    bool autoHeadroom() const { return m_autoHeadroom; }

    void setBalance(double balance);
    double balance() const { return m_balance; }

    void setChannelGain(int channel, double db);
    double channelGain(int channel) const;

    void setMono(bool on);
    bool mono() const { return m_mono; }

    void setStereoWidth(double width);
    double stereoWidth() const { return m_stereoWidth; }

    void setSwapChannels(bool on);
    bool swapChannels() const { return m_swapChannels; }

    // trackPeak：曲目峰值（0 表示未知），preventClip 用它限制增益上限
    void setReplayGain(double trackDb, double albumDb, int mode, double preampDb,
                       bool preventClip, double trackPeak);
    double replayGainDb() const;

    void setLimiter(bool on, double thresholdDb);
    bool limiterEnabled() const { return m_limiter; }

    // 总音量与淡变都在音频线程逐样本完成，不依赖 GUI 线程驱动
    void setVolume(double volume);
    void rampFade(double target, int frames);
    void jumpFade(double target);

    double outputGainDb() const;

    void process(float *data, int frames, int channels);

private:
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        void reset() { x1 = x2 = y1 = y2 = 0; }
        inline double tick(double x)
        {
            const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x;
            y2 = y1; y1 = y;
            return y;
        }
    };

    struct Band
    {
        double freq = 1000.0;
        double q = 1.4;
        double gainDb = 0.0;
        std::array<Biquad, kMaxChannels> filters;
    };

    void rebuildBand(Band &band, int channel);
    void applyChannelTools(float *data, int frames, int channels) const;
    void processVolume(float *data, int frames, int channels);

    double m_sampleRate = 48000.0;
    bool m_bypass = false;
    bool m_eqEnabled = false;
    std::array<Band, kBandCount> m_bands;
    double m_eqQ = 1.41;
    double m_preampDb = 0.0;
    bool m_autoHeadroom = true;

    double m_balance = 0.0;
    std::array<double, kMaxChannels> m_channelGainDb{0.0, 0.0};
    bool m_mono = false;
    double m_stereoWidth = 1.0;
    bool m_swapChannels = false;

    double m_rgTrackDb = 0.0;
    double m_rgAlbumDb = 0.0;
    int m_rgMode = 0;
    double m_rgPreampDb = 0.0;
    bool m_rgPreventClip = true;
    double m_trackPeak = 0.0;

    bool m_limiter = false;
    double m_limiterThreshold = 1.0;
    double m_limiterGain = 1.0;
    double m_limiterAttack = 0.0;
    double m_limiterRelease = 0.0;

    double m_volume = 1.0;
    double m_fade = 1.0;
    double m_fadeStep = 0.0;
};
