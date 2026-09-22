// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 QueMusic Contributors
//
// 音频后端离线自测：不依赖声卡，验证解码 / seek / DSP / 变速
#include "audio/AudioDsp.h"
#include "audio/AudioEngine.h"
#include "audio/AudioSpectrumSink.h"
#include "audio/FfmpegDecoder.h"
#include "audio/PitchShifter.h"
#include "audio/TimeStretch.h"

#include <QThread>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QMediaDevices>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

#include <cmath>
#include <vector>

namespace {

void writeWav(const QString &path, int sampleRate, int channels, double seconds)
{
    const int frames = int(sampleRate * seconds);
    QByteArray pcm;
    pcm.resize(frames * channels * 2);
    auto *out = reinterpret_cast<qint16 *>(pcm.data());
    for (int i = 0; i < frames; ++i) {
        const double value = 0.5 * std::sin(2.0 * M_PI * 440.0 * i / sampleRate);
        for (int c = 0; c < channels; ++c)
            out[i * channels + c] = qint16(value * 32767.0);
    }

    const int dataBytes = pcm.size();
    const int byteRate = sampleRate * channels * 2;
    QByteArray header;
    header.reserve(44);
    header.append("RIFF", 4);
    header.append(QByteArray(4, '\0'));
    header.append("WAVE", 4);
    header.append("fmt ", 4);
    const qint32 fmtSize = 16;
    header.append(reinterpret_cast<const char *>(&fmtSize), 4);
    const qint16 audioFormat = 1;
    header.append(reinterpret_cast<const char *>(&audioFormat), 2);
    const qint16 ch = qint16(channels);
    header.append(reinterpret_cast<const char *>(&ch), 2);
    header.append(reinterpret_cast<const char *>(&sampleRate), 4);
    header.append(reinterpret_cast<const char *>(&byteRate), 4);
    const qint16 blockAlign = qint16(channels * 2);
    header.append(reinterpret_cast<const char *>(&blockAlign), 2);
    const qint16 bits = 16;
    header.append(reinterpret_cast<const char *>(&bits), 2);
    header.append("data", 4);
    header.append(reinterpret_cast<const char *>(&dataBytes), 4);

    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(header);
    file.write(pcm);
    file.close();
}

bool allFinite(const std::vector<float> &data)
{
    for (float value : data) {
        if (!std::isfinite(value))
            return false;
    }
    return true;
}

} // namespace

class AudioPipelineTest : public QObject
{
    Q_OBJECT

private slots:
    void decodeAndSeek();
    void dspStaysInRange();
    void timeStretchRatio();
    void engineLoadsSource();
    void playbackRateMapping();
    void seekDoesNotRewind();
    void settingsPersist();
    void trackSwitchStress();
    void pitchShiftKeepsDuration();
    void renderOnDedicatedThread();
    void reconfigureWhilePlaying();
    void dspThroughput();
};

// 记录音频渲染实际落在哪个线程
class ThreadProbeSink : public QObject, public AudioSpectrumSink
{
public:
    std::atomic<int> calls{0};
    QThread *thread = nullptr;

    void pushSamples(const float *, int, int, int) override
    {
        thread = QThread::currentThread();
        calls.fetch_add(1, std::memory_order_release);
    }
};

void AudioPipelineTest::decodeAndSeek()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath("tone.wav");
    writeWav(path, 44100, 2, 2.0);

    FfmpegDecoder decoder;
    decoder.setTargetFormat(48000, 2);
    QString error;
    QVERIFY2(decoder.open(path, &error), qPrintable(error));
    QVERIFY(decoder.meta().durationMs >= 1900);
    QVERIFY(decoder.meta().durationMs <= 2100);
    QCOMPARE(decoder.meta().sampleRate, 44100);
    QCOMPARE(decoder.meta().channels, 2);

    std::vector<float> buffer(4096 * 2);
    qint64 total = 0;
    float peak = 0.0f;
    for (int i = 0; i < 2000; ++i) {
        const int read = decoder.readFrames(buffer.data(), 4096);
        if (read <= 0)
            break;
        QVERIFY(allFinite(buffer));
        for (int k = 0; k < read * 2; ++k)
            peak = qMax(peak, std::abs(buffer[k]));
        total += read;
    }
    QVERIFY(total > 90000);
    QVERIFY(total < 99000);
    QVERIFY(peak > 0.4f);
    QVERIFY(peak <= 1.0f);

    QVERIFY(decoder.seekTo(1000));
    const int afterSeek = decoder.readFrames(buffer.data(), 4096);
    QVERIFY(afterSeek > 0);
    QVERIFY(allFinite(buffer));
    decoder.close();
}

void AudioPipelineTest::dspStaysInRange()
{
    AudioDsp dsp;
    dsp.prepare(48000.0);

    QList<qreal> gains;
    for (int i = 0; i < AudioDsp::kBandCount; ++i)
        gains.append(i == 5 ? 12.0 : 0.0);
    dsp.setEqGains(gains);
    dsp.setEqEnabled(true);
    dsp.setPreampDb(6.0);
    dsp.setAutoHeadroom(false);
    dsp.setBalance(-0.5);
    dsp.setStereoWidth(1.6);
    dsp.setMono(false);
    dsp.setLimiter(true, -0.5);

    std::vector<float> block(1024 * 2);
    double sum = 0.0;
    for (int i = 0; i < 64; ++i) {
        for (int k = 0; k < 1024; ++k) {
            const float value = float(0.7 * std::sin(2.0 * M_PI * 1000.0 * (i * 1024 + k) / 48000.0));
            block[size_t(k) * 2] = value;
            block[size_t(k) * 2 + 1] = -value;
        }
        dsp.process(block.data(), 1024, 2);
        QVERIFY(allFinite(block));
        for (float v : block) {
            QVERIFY(v >= -1.0f && v <= 1.0f);
            sum += std::abs(v);
        }
    }
    QVERIFY(sum > 0.0);

    dsp.setEqGains(QList<qreal>(AudioDsp::kBandCount, 0.0));
    dsp.setPreampDb(0.0);
    dsp.setLimiter(false, 0.0);
    QVERIFY(AudioDsp::presetNames().contains(QStringLiteral("Rock")));
    QCOMPARE(AudioDsp::presetGains(QStringLiteral("Flat")).size(), AudioDsp::kBandCount);
}

void AudioPipelineTest::timeStretchRatio()
{
    TimeStretch stretch;
    stretch.prepare(2, 48000);

    const int inputFrames = 48000 * 6;
    std::vector<float> input(size_t(inputFrames) * 2);
    for (int i = 0; i < inputFrames; ++i) {
        const float value = float(0.5 * std::sin(2.0 * M_PI * 440.0 * i / 48000.0));
        input[size_t(i) * 2] = value;
        input[size_t(i) * 2 + 1] = value;
    }

    // 覆盖两侧极值：曾经 speed<1 时搜索窗会越界读到缓冲之前的地址
    for (double speed : {3.0, 2.0, 1.5, 0.75, 0.5, 0.25}) {
        stretch.reset();
        stretch.setSpeed(speed);
        QVERIFY(!stretch.bypass());

        int produced = 0;
        int consumed = 0;
        std::vector<float> out(4096 * 2);
        std::vector<float> in(1024 * 2);
        while (consumed < inputFrames) {
            const int give = qMin(1024, inputFrames - consumed);
            std::copy_n(input.begin() + size_t(consumed) * 2, size_t(give) * 2, in.begin());
            stretch.push(in.data(), give);
            consumed += give;
            for (;;) {
                const int got = stretch.pull(out.data(), 4096);
                if (got <= 0)
                    break;
                QVERIFY(allFinite(out));
                produced += got;
            }
        }

        const double ratio = double(consumed) / double(qMax(1, produced));
        QVERIFY2(std::abs(ratio - speed) < 0.08,
                 qPrintable(QStringLiteral("speed=%1 ratio=%2").arg(speed).arg(ratio)));
    }
}

void AudioPipelineTest::engineLoadsSource()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath("engine.wav");
    writeWav(path, 48000, 2, 1.0);

    AudioEngine engine;
    QCOMPARE(engine.mediaStatus(), AudioEngine::NoMedia);
    QCOMPARE(engine.playbackState(), AudioEngine::StoppedState);

    engine.setSource(QUrl::fromLocalFile(path));
    QCOMPARE(engine.mediaStatus(), AudioEngine::LoadingMedia);

    QTRY_VERIFY_WITH_TIMEOUT(engine.mediaStatus() == AudioEngine::LoadedMedia, 5000);
    QVERIFY(engine.duration() >= 900 && engine.duration() <= 1100);
    QCOMPARE(engine.sourceSampleRate(), 48000);
    QCOMPARE(engine.sourceChannels(), 2);
    QCOMPARE(engine.outputFormatName().isEmpty(), false);

    engine.setEqEnabled(true);
    QVariantList gains = engine.eqPresetGains(QStringLiteral("Rock"));
    QCOMPARE(gains.size(), AudioDsp::kBandCount);
    engine.setEqGains(gains);
    engine.setPreampDb(-3.0);
    engine.setLimiterEnabled(true);
    engine.setBalance(0.25);
    engine.setStereoWidth(1.2);
    QVERIFY(engine.dspActive());
    QVERIFY(engine.effectiveGainDb() < 1.0);
    QVERIFY(engine.eqPresetNames().contains(QStringLiteral("Vocal")));
    QCOMPARE(engine.eqBandLabels().size(), AudioDsp::kBandCount);

    engine.setPitchCompensation(true);
    engine.setPlaybackRate(1.25);
    QCOMPARE(engine.playbackRate(), 1.25);

    engine.setPlaybackRate(1.0);
    engine.setPitchCompensation(false);
    engine.setPosition(0);
    engine.stop();
    QCOMPARE(engine.mediaStatus(), AudioEngine::LoadedMedia);
}

void AudioPipelineTest::playbackRateMapping()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath("rate.wav");
    writeWav(path, 48000, 2, 1.0);

    AudioEngine engine;
    engine.setSource(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(engine.mediaStatus() == AudioEngine::LoadedMedia, 5000);

    const int deviceRate = engine.outputSampleRate();
    QVERIFY(deviceRate > 0);

    // 解码输出采样率必须与倍速成反比，否则倍速会反向
    for (double rate : {0.5, 0.8, 1.25, 2.0, 3.0}) {
        engine.setPlaybackRate(rate);
        const int expect = int(qint64(deviceRate) * 1000 / int(std::lround(rate * 1000.0)));
        QTRY_COMPARE_WITH_TIMEOUT(engine.decodeSampleRate(), expect, 4000);
    }

    // 开启音高补偿后解码回到原速，变速交给 SOLA
    engine.setPitchCompensation(true);
    engine.setPlaybackRate(2.0);
    QTRY_COMPARE_WITH_TIMEOUT(engine.decodeSampleRate(), deviceRate, 4000);

    engine.setPitchCompensation(false);
    engine.setPlaybackRate(1.0);
    engine.stop();
}

// 进度条"往回闪"的回归保护：seek 落地前不得发布旧位置
void AudioPipelineTest::seekDoesNotRewind()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath("seek.wav");
    writeWav(path, 48000, 2, 8.0);

    AudioEngine engine;
    engine.setVolume(0.0);
    engine.setSource(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(engine.mediaStatus() == AudioEngine::LoadedMedia, 5000);
    engine.play();
    QTest::qWait(500);
    if (engine.position() <= 0)
        QSKIP("当前环境无可用输出设备，跳过实时进度校验");

    const qint64 target = 4000;
    engine.setPosition(target);
    QCOMPARE(engine.position(), target);

    qint64 minSeen = target;
    for (int i = 0; i < 80; ++i) {
        QTest::qWait(20);
        minSeen = qMin(minSeen, engine.position());
    }
    QVERIFY2(minSeen >= target - 200,
             qPrintable(QStringLiteral("position rewound to %1 (target %2)").arg(minSeen).arg(target)));
    QVERIFY(engine.position() >= target);

    engine.stop();
}

void AudioPipelineTest::settingsPersist()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // 把配置重定向到临时目录，避免污染真实音频设置
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());

    {
        AudioEngine engine;
        engine.setEqEnabled(true);
        engine.setEqBand(3, 9.0);
        engine.setEqQ(2.5);
        engine.setPreampDb(-4.5);
        engine.setStereoWidth(1.4);
        engine.setLimiterEnabled(true);
        engine.setDspEnabled(false);
    }

    AudioEngine restored;
    QCOMPARE(restored.eqEnabled(), true);
    QCOMPARE(restored.eqGains().at(3).toDouble(), 9.0);
    QCOMPARE(restored.eqQ(), 2.5);
    QCOMPARE(restored.preampDb(), -4.5);
    QCOMPARE(restored.stereoWidth(), 1.4);
    QCOMPARE(restored.limiterEnabled(), true);
    QCOMPARE(restored.dspEnabled(), false);
}

// 切歌压力：连续换源 + seek + 自然结束，覆盖数据段切换与并发路径
void AudioPipelineTest::trackSwitchStress()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = QDir(dir.path()).filePath("a.wav");
    const QString b = QDir(dir.path()).filePath("b.wav");
    writeWav(a, 48000, 2, 0.5);
    writeWav(b, 44100, 2, 0.5);

    AudioEngine engine;
    engine.setVolume(0.0);

    for (int i = 0; i < 40; ++i) {
        engine.setSource(QUrl::fromLocalFile((i % 2) ? a : b));
        engine.play();
        QTest::qWait(60);
        engine.setPosition(150 + i * 5);
        QTest::qWait(30);
    }

    engine.setSource(QUrl::fromLocalFile(a));
    engine.play();
    QTRY_VERIFY_WITH_TIMEOUT(engine.mediaStatus() == AudioEngine::EndOfMedia, 6000);
    engine.setSource(QUrl::fromLocalFile(b));
    engine.play();
    QTest::qWait(300);
    QVERIFY(engine.mediaStatus() != AudioEngine::InvalidMedia);
    engine.stop();
}

// 变调不变速：时长不变、频率随音高倍率变化
void AudioPipelineTest::pitchShiftKeepsDuration()
{
    const int rate = 48000;
    const int inputFrames = rate * 3;
    std::vector<float> in(size_t(inputFrames) * 2);
    for (int i = 0; i < inputFrames; ++i) {
        const float v = float(0.5 * std::sin(2.0 * M_PI * 440.0 * i / rate));
        in[size_t(i) * 2] = v;
        in[size_t(i) * 2 + 1] = v;
    }

    std::vector<float> out(1024 * 2);

    for (double pitch : {2.0, 0.5}) {
        PitchShifter shifter;
        shifter.prepare(2, rate);
        shifter.setRatios(1.0, pitch);
        QVERIFY(!shifter.bypass());

        int produced = 0;
        int crossings = 0;
        int prevSign = 0;
        auto drain = [&] {
            for (;;) {
                const int got = shifter.pull(out.data(), 1024);
                if (got <= 0)
                    break;
                for (int i = 0; i < got; ++i) {
                    const float v = out[size_t(i) * 2];
                    const int sign = v > 0.0f ? 1 : (v < 0.0f ? -1 : 0);
                    if (sign && prevSign && sign != prevSign)
                        ++crossings;
                    if (sign)
                        prevSign = sign;
                }
                produced += got;
            }
        };

        for (int off = 0; off < inputFrames; off += 1024) {
            const int give = qMin(1024, inputFrames - off);
            shifter.push(in.data() + size_t(off) * 2, give);
            drain();
        }

        // 链内会残留不足一个分析窗的数据，允许 10% 的尾部损失
        const double duration = double(produced) / double(inputFrames);
        QVERIFY2(std::abs(duration - 1.0) < 0.10,
                 qPrintable(QStringLiteral("pitch=%1 duration=%2").arg(pitch).arg(duration)));

        const double freq = double(crossings) / 2.0 * double(rate) / double(qMax(1, produced));
        const double expect = 440.0 * pitch;
        QVERIFY2(std::abs(freq - expect) / expect < 0.12,
                 qPrintable(QStringLiteral("pitch=%1 freq=%2 expect=%3").arg(pitch).arg(freq).arg(expect)));
    }
}

// 音频渲染必须跑在独立线程：曾经落在 GUI 线程上，拖窗口改尺寸就会饿死音频
void AudioPipelineTest::renderOnDedicatedThread()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath("thread.wav");
    writeWav(path, 48000, 2, 3.0);

    AudioEngine engine;
    engine.setVolume(0.0);

    ThreadProbeSink sink;
    engine.setSpectrumSink(&sink);
    engine.setSource(QUrl::fromLocalFile(path));
    QTRY_VERIFY_WITH_TIMEOUT(engine.mediaStatus() == AudioEngine::LoadedMedia, 5000);
    engine.play();

    QTRY_VERIFY_WITH_TIMEOUT(sink.calls.load(std::memory_order_acquire) > 0, 3000);
    QVERIFY2(sink.thread != QThread::currentThread(),
             "音频渲染仍在 GUI 线程上执行，GUI 卡顿会直接导致断音");

    engine.setSpectrumSink(nullptr);
    engine.stop();
}

// 播放中重建输出缓冲：resize/configure 会释放旧内存，而音频线程手里还有旧指针
void AudioPipelineTest::reconfigureWhilePlaying()
{
    if (QMediaDevices::defaultAudioOutput().isNull())
        QSKIP("无音频输出设备");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString a = dir.filePath("a.wav");
    const QString b = dir.filePath("b.wav");
    writeWav(a, 48000, 2, 4.0);
    writeWav(b, 44100, 2, 4.0);

    AudioEngine engine;
    engine.setVolume(0.0);
    engine.setSource(QUrl::fromLocalFile(a));
    QTRY_VERIFY_WITH_TIMEOUT(engine.mediaStatus() == AudioEngine::LoadedMedia, 5000);
    engine.play();

    for (int i = 0; i < 8; ++i) {
        engine.setBufferMs(30 + (i % 4) * 20);
        QTest::qWait(60);
        engine.setOutputSampleRate(i % 3 == 0 ? 44100 : (i % 3 == 1 ? 48000 : 0));
        QTest::qWait(60);
        engine.setSource(QUrl::fromLocalFile(i % 2 ? b : a));
        QTest::qWait(90);
        engine.play();
    }

    engine.setSource(QUrl::fromLocalFile(a));
    QTRY_VERIFY_WITH_TIMEOUT(engine.mediaStatus() == AudioEngine::LoadedMedia, 5000);
    engine.play();
    QTest::qWait(300);
    engine.stop();
}

// DSP 热路径基准：EQ 十段全开，输出 ns/帧用于对比优化前后
void AudioPipelineTest::dspThroughput()
{
    AudioDsp dsp;
    dsp.prepare(48000.0);
    dsp.setEqEnabled(true);
    QList<qreal> gains;
    for (int i = 0; i < AudioDsp::kBandCount; ++i)
        gains.append((i % 2) ? 3.0 : -3.0);
    dsp.setEqGains(gains);
    dsp.setVolume(0.6);

    const int frames = 4096;
    const int channels = 2;
    const int blocks = 400;
    std::vector<float> buf(size_t(frames) * size_t(channels), 0.1f);

    QElapsedTimer timer;
    timer.start();
    for (int b = 0; b < blocks; ++b) {
        buf[0] = (b % 2) ? 0.1f : -0.1f;   // 避免被优化掉
        dsp.process(buf.data(), frames, channels);
    }
    const double nsPerFrame = double(timer.nsecsElapsed()) / (double(blocks) * frames);
    qInfo("[BENCH] EQ10 %dch: %.1f ns/frame, 约占单核 %.2f%%（48000 帧/秒）",
          channels, nsPerFrame, nsPerFrame * 48000.0 / 1e7);
    QVERIFY2(nsPerFrame < 5000.0, "DSP 每帧耗时异常，热路径可能劣化");
}

QTEST_MAIN(AudioPipelineTest)
#include "audio_pipeline_test.moc"
