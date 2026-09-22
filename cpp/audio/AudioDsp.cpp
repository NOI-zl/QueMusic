// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 QueMusic Contributors
//
#include "AudioDsp.h"

#include <QtGlobal>

#include <cmath>

namespace {

const double kBandFreqs[AudioDsp::kBandCount] = {
    31.0, 62.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0
};

const QString kBandNames[AudioDsp::kBandCount] = {
    "31", "62", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"
};

struct Preset
{
    const char *name;
    double gains[AudioDsp::kBandCount];
};

const Preset kPresets[] = {
    { "Flat",         {  0,   0,   0,   0,   0,   0,   0,   0,   0,   0 } },
    { "Rock",         {  5,   4,   2,  -1,  -2,  -1,   1,   3,   4,   5 } },
    { "Pop",          { -1,   1,   3,   4,   3,   1,  -1,  -1,   1,   2 } },
    { "Jazz",         {  3,   2,   1,   2,  -1,  -1,   0,   1,   2,   3 } },
    { "Classical",    {  4,   3,   2,   0,   0,   0,  -1,  -2,  -3,  -4 } },
    { "Dance",        {  6,   5,   3,   0,   1,   2,   3,   4,   4,   3 } },
    { "Bass Boost",   {  8,   7,   5,   3,   1,   0,   0,   0,   0,   0 } },
    { "Treble Boost", {  0,   0,   0,   0,   0,   1,   3,   5,   6,   7 } },
    { "Vocal",        { -3,  -2,  -1,   2,   4,   5,   4,   2,   0,  -1 } },
    { "Loudness",     {  6,   4,   0,  -2,  -3,  -2,   0,   2,   5,   6 } }
};

inline double dbToLinear(double db) { return std::pow(10.0, db / 20.0); }

} // namespace

const double *AudioDsp::bandFrequencies() { return kBandFreqs; }

QStringList AudioDsp::bandLabels()
{
    QStringList labels;
    labels.reserve(kBandCount);
    for (int i = 0; i < kBandCount; ++i)
        labels.append(kBandNames[i]);
    return labels;
}

QStringList AudioDsp::presetNames()
{
    QStringList names;
    const int count = int(sizeof(kPresets) / sizeof(kPresets[0]));
    names.reserve(count);
    for (int i = 0; i < count; ++i)
        names.append(QString::fromLatin1(kPresets[i].name));
    return names;
}

QList<qreal> AudioDsp::presetGains(const QString &name)
{
    const int count = int(sizeof(kPresets) / sizeof(kPresets[0]));
    for (int i = 0; i < count; ++i) {
        if (name.compare(QString::fromLatin1(kPresets[i].name), Qt::CaseInsensitive) != 0)
            continue;
        QList<qreal> gains;
        gains.reserve(kBandCount);
        for (int b = 0; b < kBandCount; ++b)
            gains.append(kPresets[i].gains[b]);
        return gains;
    }
    return QList<qreal>(kBandCount, 0.0);
}

AudioDsp::AudioDsp()
{
    for (int i = 0; i < kBandCount; ++i) {
        m_bands[i].freq = kBandFreqs[i];
        m_bands[i].q = m_eqQ;
    }
    prepare(m_sampleRate);
}

void AudioDsp::prepare(double sampleRate)
{
    if (sampleRate > 0)
        m_sampleRate = sampleRate;
    m_limiterAttack = std::exp(-1.0 / (0.0015 * m_sampleRate));
    m_limiterRelease = std::exp(-1.0 / (0.060 * m_sampleRate));
    reset();
    for (int i = 0; i < kBandCount; ++i)
        for (int c = 0; c < kMaxChannels; ++c)
            rebuildBand(m_bands[i], c);
}

void AudioDsp::reset()
{
    for (int i = 0; i < kBandCount; ++i)
        for (int c = 0; c < kMaxChannels; ++c)
            m_bands[i].filters[c].reset();
    m_limiterGain = 1.0;
}

void AudioDsp::rebuildBand(Band &band, int channel)
{
    Biquad &f = band.filters[channel];
    if (band.gainDb == 0.0) {
        f = Biquad();
        return;
    }

    const double w0 = 2.0 * M_PI * qBound(10.0, band.freq, m_sampleRate * 0.49) / m_sampleRate;
    const double cosW = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * qMax(0.1, band.q));
    const double amp = std::pow(10.0, band.gainDb / 40.0);

    const double a0 = 1.0 + alpha / amp;
    f.b0 = (1.0 + alpha * amp) / a0;
    f.b1 = (-2.0 * cosW) / a0;
    f.b2 = (1.0 - alpha * amp) / a0;
    f.a1 = (-2.0 * cosW) / a0;
    f.a2 = (1.0 - alpha / amp) / a0;
}

void AudioDsp::setEqEnabled(bool on)
{
    if (m_eqEnabled == on)
        return;
    m_eqEnabled = on;
    reset();
}

void AudioDsp::setEqGains(const QList<qreal> &gainsDb)
{
    double gains[kBandCount] = {0};
    const int n = qMin(gainsDb.size(), kBandCount);
    for (int i = 0; i < n; ++i)
        gains[i] = qBound(-24.0, double(gainsDb.at(i)), 24.0);
    setEqGains(gains, n);
}

void AudioDsp::setEqGains(const double *gainsDb, int count)
{
    const int n = qMin(count, kBandCount);
    for (int i = 0; i < n; ++i) {
        const double g = qBound(-24.0, gainsDb[i], 24.0);
        if (m_bands[i].gainDb == g)
            continue;
        m_bands[i].gainDb = g;
        for (int c = 0; c < kMaxChannels; ++c)
            rebuildBand(m_bands[i], c);
    }
}

QList<qreal> AudioDsp::eqGains() const
{
    QList<qreal> gains;
    gains.reserve(kBandCount);
    for (int i = 0; i < kBandCount; ++i)
        gains.append(m_bands[i].gainDb);
    return gains;
}

void AudioDsp::setEqQ(double q)
{
    const double value = qBound(0.2, q, 8.0);
    if (qFuzzyCompare(m_eqQ, value))
        return;
    m_eqQ = value;
    for (int i = 0; i < kBandCount; ++i) {
        m_bands[i].q = value;
        for (int c = 0; c < kMaxChannels; ++c)
            rebuildBand(m_bands[i], c);
    }
}

void AudioDsp::setPreampDb(double db)
{
    m_preampDb = qBound(-24.0, db, 24.0);
}

void AudioDsp::setAutoHeadroom(bool on)
{
    m_autoHeadroom = on;
}

void AudioDsp::setBalance(double balance)
{
    m_balance = qBound(-1.0, balance, 1.0);
}

void AudioDsp::setChannelGain(int channel, double db)
{
    if (channel < 0 || channel >= kMaxChannels)
        return;
    m_channelGainDb[channel] = qBound(-24.0, db, 24.0);
}

double AudioDsp::channelGain(int channel) const
{
    return (channel >= 0 && channel < kMaxChannels) ? m_channelGainDb[channel] : 0.0;
}

void AudioDsp::setMono(bool on) { m_mono = on; }

void AudioDsp::setStereoWidth(double width) { m_stereoWidth = qBound(0.0, width, 2.0); }

void AudioDsp::setSwapChannels(bool on) { m_swapChannels = on; }

void AudioDsp::setVolume(double volume)
{
    m_volume = qBound(0.0, volume, 1.0);
}

void AudioDsp::rampFade(double target, int frames)
{
    const double value = qBound(0.0, target, 1.0);
    if (frames <= 0) {
        jumpFade(value);
        return;
    }
    m_fadeStep = (value - m_fade) / frames;
    if (qFuzzyIsNull(m_fadeStep))
        m_fade = value;
}

void AudioDsp::jumpFade(double target)
{
    m_fade = qBound(0.0, target, 1.0);
    m_fadeStep = 0.0;
}

void AudioDsp::setLimiter(bool on, double thresholdDb)
{
    m_limiter = on;
    m_limiterThreshold = qBound(0.05, std::pow(10.0, thresholdDb / 20.0), 1.0);
    if (!on)
        m_limiterGain = 1.0;
}

void AudioDsp::setReplayGain(double trackDb, double albumDb, int mode, double preampDb,
                             bool preventClip, double trackPeak)
{
    m_rgTrackDb = trackDb;
    m_rgAlbumDb = albumDb;
    m_rgMode = mode;
    m_rgPreampDb = preampDb;
    m_rgPreventClip = preventClip;
    m_trackPeak = trackPeak;
}

double AudioDsp::replayGainDb() const
{
    if (m_rgMode <= 0)
        return 0.0;
    const double base = (m_rgMode == 2) ? m_rgAlbumDb : m_rgTrackDb;
    double gain = base + m_rgPreampDb;
    if (m_rgPreventClip && m_trackPeak > 0.0)
        gain = qMin(gain, -20.0 * std::log10(m_trackPeak));
    return gain;
}

double AudioDsp::outputGainDb() const
{
    double headroom = 0.0;
    if (m_autoHeadroom && m_eqEnabled) {
        double maxGain = 0.0;
        for (int i = 0; i < kBandCount; ++i)
            maxGain = qMax(maxGain, m_bands[i].gainDb);
        headroom = qMax(0.0, maxGain);
    }
    return m_preampDb + replayGainDb() - headroom;
}

void AudioDsp::applyChannelTools(float *data, int frames, int channels) const
{
    if (channels < 2) {
        const double g0 = dbToLinear(m_channelGainDb[0]);
        const double bal = (m_balance > 0.0) ? (1.0 - m_balance) : 1.0;
        const double k = g0 * bal;
        if (k != 1.0)
            for (int i = 0; i < frames; ++i)
                data[i] *= float(k);
        return;
    }

    double gainL = dbToLinear(m_channelGainDb[0]);
    double gainR = dbToLinear(m_channelGainDb[1]);
    if (m_balance > 0.0)
        gainL *= (1.0 - m_balance);
    else if (m_balance < 0.0)
        gainR *= (1.0 + m_balance);

    const double width = m_stereoWidth;
    const bool needsTools = m_mono || m_swapChannels || width != 1.0
                            || !qFuzzyCompare(gainL, 1.0) || !qFuzzyCompare(gainR, 1.0);
    if (!needsTools)
        return;

    for (int i = 0; i < frames; ++i) {
        float &l = data[i * channels];
        float &r = data[i * channels + 1];
        if (m_mono) {
            const float mid = (l + r) * 0.5f;
            l = mid;
            r = mid;
        }
        if (width != 1.0) {
            const double mid = (double(l) + double(r)) * 0.5;
            const double side = (double(l) - double(r)) * 0.5 * width;
            l = float(mid + side);
            r = float(mid - side);
        }
        if (m_swapChannels)
            std::swap(l, r);
        l = float(l * gainL);
        r = float(r * gainR);
    }
}

void AudioDsp::process(float *data, int frames, int channels)
{
    if (frames <= 0 || channels <= 0)
        return;

    if (m_bypass)
        return processVolume(data, frames, channels);

    if (m_eqEnabled) {
        const int ch = qMin(channels, kMaxChannels);
        // 单次遍历完成所有启用频段，避免按频段反复扫过整个缓冲
        Band *active[kBandCount];
        int activeCount = 0;
        for (int b = 0; b < kBandCount; ++b)
            if (m_bands[b].gainDb != 0.0)
                active[activeCount++] = &m_bands[b];

        for (int i = 0; i < frames; ++i) {
            float *frame = data + size_t(i) * channels;
            for (int c = 0; c < ch; ++c) {
                double v = frame[c];
                for (int b = 0; b < activeCount; ++b)
                    v = active[b]->filters[c].tick(v);
                frame[c] = float(v);
            }
        }
    }

    const double gain = dbToLinear(outputGainDb());
    if (!qFuzzyCompare(gain, 1.0))
        for (int i = 0, n = frames * channels; i < n; ++i)
            data[i] = float(double(data[i]) * gain);

    applyChannelTools(data, frames, channels);

    if (m_limiter) {
        const double thr = m_limiterThreshold;
        const double a = m_limiterAttack;
        const double r = m_limiterRelease;
        for (int i = 0; i < frames; ++i) {
            double peak = 0.0;
            for (int c = 0; c < channels; ++c)
                peak = qMax(peak, std::abs(double(data[i * channels + c])));
            const double target = (peak > thr) ? (thr / peak) : 1.0;
            m_limiterGain = (target < m_limiterGain)
                                ? (a * m_limiterGain + (1.0 - a) * target)
                                : (r * m_limiterGain + (1.0 - r) * target);
            const double g = m_limiterGain;
            for (int c = 0; c < channels; ++c) {
                const double v = double(data[i * channels + c]) * g;
                data[i * channels + c] = float(qBound(-1.0, v, 1.0));
            }
        }
    } else {
        for (int i = 0, n = frames * channels; i < n; ++i)
            data[i] = float(qBound(-1.0, double(data[i]), 1.0));
    }

    processVolume(data, frames, channels);
}

// 总音量与淡变：逐样本推进，不受 GUI 线程调度影响
void AudioDsp::processVolume(float *data, int frames, int channels)
{
    if (m_fadeStep != 0.0) {
        for (int i = 0; i < frames; ++i) {
            const double g = m_volume * m_fade;
            float *frame = data + size_t(i) * channels;
            for (int c = 0; c < channels; ++c)
                frame[c] = float(double(frame[c]) * g);
            m_fade += m_fadeStep;
            if (m_fade <= 0.0) {
                m_fade = 0.0;
                m_fadeStep = 0.0;
            } else if (m_fade >= 1.0) {
                m_fade = 1.0;
                m_fadeStep = 0.0;
            }
        }
    } else {
        const double g = m_volume * m_fade;
        if (!qFuzzyCompare(g, 1.0))
            for (int i = 0, n = frames * channels; i < n; ++i)
                data[i] = float(double(data[i]) * g);
    }
}
