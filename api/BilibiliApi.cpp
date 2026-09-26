// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 QueMusic Contributors
//
#include "BilibiliApi.h"

#include "ApiCommon.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <climits>

namespace {
const char *kUa = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                  "(KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
const char *kReferer = "https://www.bilibili.com";
const QString kApi = QStringLiteral("https://api.bilibili.com");

// WBI 混淆表（bilibili-API-collect 公开算法）
const int kMixinTab[64] = {46, 47, 18, 2, 53, 8, 23, 32, 15, 50, 10, 31, 58, 3, 45, 35,
                           27, 43, 5, 49, 33, 9, 42, 19, 29, 28, 14, 39, 12, 38, 41, 13,
                           37, 48, 7, 16, 24, 55, 40, 61, 26, 17, 0, 1, 60, 51, 30, 4,
                           22, 25, 54, 21, 56, 59, 6, 63, 57, 62, 11, 36, 20, 34, 44, 52};

// 音乐分区：名称用于分类检索关键词，tid 用于榜单标识
struct Region {
    const char *name;
    int tid;
};
const Region kRegions[] = {
    {"音乐综合", 3},   {"原创音乐", 28}, {"翻唱", 31},      {"VOCALOID", 30},
    {"演奏", 59},      {"电音", 194},    {"三次元音乐", 29}, {"乐评盘点", 243},
    {"AI音乐", 265},
};

QString regionName(int tid)
{
    for (const Region &r : kRegions) {
        if (r.tid == tid)
            return QString::fromUtf8(r.name);
    }
    return QString();
}

// 搜索标题带 <em class="keyword"> 高亮标签
QString cleanTitle(QString t)
{
    t.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    return t.trimmed();
}

QString httpsUrl(QString u)
{
    if (u.startsWith(QLatin1String("//")))
        return QStringLiteral("https:") + u;
    if (u.startsWith(QLatin1String("http://")))
        u.replace(0, 7, QStringLiteral("https://"));
    return u;
}

// 搜索接口给 "m:ss"/"h:mm:ss"，排行与详情接口直接给秒
int durationSeconds(const QJsonValue &v)
{
    if (v.isDouble())
        return v.toInt();
    int total = 0;
    for (const QString &part : v.toString().split(QLatin1Char(':')))
        total = total * 60 + part.toInt();
    return total;
}

// 下载文件名扩展名：DASH 音频流固定是 m4a；
// durl 回退是音视频合流，容器可能是 mp4 / flv 等，故优先取 URL 路径里的扩展名，
// 不在白名单内的（含路径里偶然出现的点号）一律退回默认值。
QString mediaSuffix(const QString &url, bool durlFallback)
{
    if (!durlFallback)
        return QStringLiteral("m4a");
    const QString suffix =
        QUrl(url).path().section(QLatin1Char('.'), -1).toLower();
    static const QSet<QString> kKnownContainers = {
        QStringLiteral("m4a"), QStringLiteral("mp4"), QStringLiteral("m4s"),
        QStringLiteral("flv"), QStringLiteral("aac"), QStringLiteral("mp3"),
        QStringLiteral("ts"),  QStringLiteral("mov"), QStringLiteral("mkv"),
        QStringLiteral("webm"),
    };
    return kKnownContainers.contains(suffix) ? suffix : QStringLiteral("mp4");
}

// 目标音质对应的 B 站音频 id：30216 标准 / 30280 高清 / 30251 Hi-Res
QJsonObject pickAudio(const QJsonArray &audios, int quality)
{
    if (audios.isEmpty()) // 无音频流时返回空对象，调用方按“无可用地址”处理
        return QJsonObject();
    const int target = quality >= 2 ? 30251 : (quality == 1 ? 30280 : 30216);
    QJsonObject best = audios.first().toObject();
    int bestScore = INT_MAX;
    for (const QJsonValue &v : audios) {
        const QJsonObject o = v.toObject();
        const int score = qAbs(o.value(QStringLiteral("id")).toInt() - target);
        if (score < bestScore) {
            bestScore = score;
            best = o;
        }
    }
    return best;
}

} // namespace

BilibiliApi::BilibiliApi(QObject *parent)
    : QObject(parent)
{
    m_nam = new QNetworkAccessManager(this);
}

QString BilibiliApi::mixinKey(const QString &imgKey, const QString &subKey)
{
    const QString raw = imgKey + subKey;
    if (raw.size() < 64)
        return QString();
    QString out;
    out.reserve(32);
    for (int i = 0; i < 32; ++i)
        out.append(raw.at(kMixinTab[i]));
    return out;
}

void BilibiliApi::get(const QString &url, const Callback &cb)
{
    QNetworkRequest req{QUrl(url)};
    req.setRawHeader("User-Agent", kUa);
    req.setRawHeader("Referer", kReferer);
    req.setRawHeader("Accept-Encoding", "identity");
    if (!m_buvid3.isEmpty()) {
        QString cookie = QStringLiteral("buvid3=%1; buvid4=%2").arg(m_buvid3, m_buvid4);
        if (!m_loginCookie.isEmpty())
            cookie += QLatin1Char(';') + m_loginCookie;
        req.setRawHeader("Cookie", cookie.toUtf8());
    }

    QNetworkReply *reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [reply, cb] {
        const QByteArray body = reply->readAll();
        // 非 2xx 统一用 UnknownContentError 表达“响应内容不可用”（HTTP 头本身就是错误信息，
        // 正文多为 HTML 错误页）：Qt 对 HTTP 错误通常已给出对应错误码，
        // 但本地代理 / 重定向场景下 reply->error() 可能仍是 NoError。
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QNetworkReply::NetworkError error = reply->error();
        QJsonObject obj;
        if (error != QNetworkReply::NoError) {
            qWarning() << "[bilibili] 请求失败:" << error << reply->errorString()
                       << reply->url().toString();
        } else if (status > 0 && (status < 200 || status >= 300)) {
            qWarning() << "[bilibili] HTTP 状态异常:" << status << reply->url().toString();
            error = QNetworkReply::UnknownContentError;
        } else {
            // JSON 不可解析同样算失败：否则损坏的响应会被上层当成“接口正常返回空数据”
            QJsonParseError parseError{};
            const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
            if (parseError.error != QJsonParseError::NoError) {
                qWarning() << "[bilibili] 响应解析失败:" << parseError.errorString()
                           << reply->url().toString();
                error = QNetworkReply::ProtocolFailure;
            } else {
                obj = doc.object();
            }
        }
        reply->deleteLater();
        // 失败时照样回调（obj 为空、error 为真实原因），保证上层 loadState 能复位
        cb(obj, error);
    });
}

void BilibiliApi::ensureKeys(const Task &then, const KeyWaiter &onFail)
{
    if (!m_mixinKey.isEmpty() && !m_buvid3.isEmpty()) {
        then();
        return;
    }
    if (m_keyRequesting) {
        // 排队等待：成功才执行 then，失败交给 onFail（不再静默抽干队列）
        m_keyWaiters.append([then, onFail](QNetworkReply::NetworkError error) {
            if (error == QNetworkReply::NoError) {
                if (then)
                    then();
            } else if (onFail) {
                onFail(error);
            }
        });
        return;
    }
    m_keyRequesting = true;

    // 密钥链任一环节失败：复位状态、清空密钥、丢弃等待队列（排队的请求一律不发），
    // 并逐个通知失败，让上层能发出带 error 的空结果而不是无声无息。
    const auto failAll = [this](QNetworkReply::NetworkError error) {
        m_keyRequesting = false;
        m_mixinKey.clear();
        m_buvid3.clear();
        m_buvid4.clear();
        const QList<KeyWaiter> waiters = m_keyWaiters;
        m_keyWaiters.clear();
        if (!waiters.isEmpty())
            qWarning() << "[bilibili] 密钥获取失败，丢弃等待中的请求:" << waiters.size()
                       << error;
        for (const KeyWaiter &w : waiters)
            w(error);
    };

    get(QStringLiteral("https://api.bilibili.com/x/web-interface/nav"),
        [this, failAll](const QJsonObject &j, QNetworkReply::NetworkError error) {
            if (error != QNetworkReply::NoError) {
                qWarning() << "[bilibili] WBI 密钥请求失败:" << error;
                failAll(error);
                return;
            }
            const QJsonObject img = j.value(QStringLiteral("data")).toObject()
                                        .value(QStringLiteral("wbi_img")).toObject();
            const auto baseName = [](const QString &u) {
                return u.section(QLatin1Char('/'), -1).section(QLatin1Char('.'), 0, 0);
            };
            const QString key = mixinKey(baseName(img.value(QStringLiteral("img_url")).toString()),
                                         baseName(img.value(QStringLiteral("sub_url")).toString()));
            if (key.isEmpty()) { // 响应正常但没有可用的 img_key/sub_key：按内容错误处理
                qWarning() << "[bilibili] WBI 密钥响应缺少 img_key/sub_key";
                failAll(QNetworkReply::UnknownContentError);
                return;
            }
            m_mixinKey = key;
            get(QStringLiteral("https://api.bilibili.com/x/frontend/finger/spi"),
                [this, failAll](const QJsonObject &j2, QNetworkReply::NetworkError error2) {
                    if (error2 != QNetworkReply::NoError) {
                        qWarning() << "[bilibili] buvid 请求失败:" << error2;
                        failAll(error2);
                        return;
                    }
                    const QJsonObject d = j2.value(QStringLiteral("data")).toObject();
                    m_buvid3 = d.value(QStringLiteral("b_3")).toString();
                    m_buvid4 = d.value(QStringLiteral("b_4")).toString();
                    if (m_buvid3.isEmpty()) { // 没有指纹时后续搜索会被风控拦截，视为失败
                        qWarning() << "[bilibili] buvid 响应缺少 b_3";
                        failAll(QNetworkReply::UnknownContentError);
                        return;
                    }
                    m_keyRequesting = false;
                    const QList<KeyWaiter> waiters = m_keyWaiters;
                    m_keyWaiters.clear();
                    for (const KeyWaiter &w : waiters)
                        w(QNetworkReply::NoError);
                });
        });
}

void BilibiliApi::getSigned(const QString &path, QVariantMap params, const Callback &cb)
{
    // 密钥链失败：不排队、直接以空对象 + 错误码回调，调用方会把它转成带 error 的空结果
    const KeyWaiter onFail = [cb](QNetworkReply::NetworkError error) {
        if (cb)
            cb(QJsonObject(), error);
    };
    ensureKeys([this, path, params, cb]() mutable {
        if (m_mixinKey.isEmpty()) { // 理论上不会走到（ensureKeys 已拦截），仍按失败处理
            if (cb)
                cb(QJsonObject(), QNetworkReply::UnknownContentError);
            return;
        }
        params.insert(QStringLiteral("wts"),
                      QString::number(QDateTime::currentSecsSinceEpoch()));
        QStringList keys = params.keys();
        std::sort(keys.begin(), keys.end());
        QStringList pairs;
        for (const QString &k : keys) {
            QString v = params.value(k).toString();
            v.remove(QRegularExpression(QStringLiteral("[!'()*]")));
            pairs << QStringLiteral("%1=%2")
                         .arg(QString::fromUtf8(QUrl::toPercentEncoding(k)),
                              QString::fromUtf8(QUrl::toPercentEncoding(v)));
        }
        const QString query = pairs.join(QLatin1Char('&'));
        const QByteArray sign = QCryptographicHash::hash((query + m_mixinKey).toUtf8(),
                                                         QCryptographicHash::Md5).toHex();
        get(kApi + path + QLatin1Char('?') + query + QStringLiteral("&w_rid=")
                + QString::fromLatin1(sign),
            cb);
    }, onFail);
}

QVariantMap BilibiliApi::toSong(const QJsonObject &v)
{
    const QJsonObject owner = v.value(QStringLiteral("owner")).toObject();
    const QVariant mid = owner.contains(QStringLiteral("mid"))
                             ? owner.value(QStringLiteral("mid")).toVariant()
                             : v.value(QStringLiteral("mid")).toVariant();
    QString artist = owner.value(QStringLiteral("name")).toString();
    if (artist.isEmpty())
        artist = v.value(QStringLiteral("author")).toString();
    qint64 play = v.value(QStringLiteral("stat")).toObject()
                      .value(QStringLiteral("view")).toVariant().toLongLong();
    if (play <= 0)
        play = v.value(QStringLiteral("play")).toVariant().toLongLong();
    if (!artist.isEmpty() && !mid.toString().isEmpty())
        m_upNames.insert(mid.toString(), artist); // 歌手页后续按 mid 取该 UP 的投稿
    return ApiCommon::song(cleanTitle(v.value(QStringLiteral("title")).toString()), artist,
                           httpsUrl(v.value(QStringLiteral("pic")).toString()),
                           v.value(QStringLiteral("bvid")).toString(),
                           durationSeconds(v.value(QStringLiteral("duration"))),
                           QString(), QString(), QString(), 0, play);
}

void BilibiliApi::emitList(const QString &action, const QVariantList &info)
{
    emit resultReady(action, ApiCommon::listResult(info), Source);
}

void BilibiliApi::emitLyrics(const QString &hash, const QVariantList &lyrics)
{
    QVariantMap data;
    data.insert(QStringLiteral("info"), lyrics);
    data.insert(QStringLiteral("translate"), QVariantList());
    data.insert(QStringLiteral("hash"), hash);
    emit resultReady(QStringLiteral("getLyricInfo"), data, Source);
}

// 失败上报：发出“空但结构完整”的 payload（沿用列表协议 { info: [] }），
// 并附加 "error" 字段（int，QNetworkReply::NetworkError 值），
// 让上层能区分“请求失败”和“接口正常返回空数据”；extra 用于补上 action 自身的必需字段。
void BilibiliApi::emitError(const QString &action, QNetworkReply::NetworkError error,
                            const QVariantMap &extra)
{
    QVariantMap data = ApiCommon::listResult({});
    for (auto it = extra.constBegin(); it != extra.constEnd(); ++it)
        data.insert(it.key(), it.value());
    data.insert(QStringLiteral("error"), int(error));
    emit resultReady(action, data, Source);
}

// 歌词协议要求带 hash / translate，失败时同样要带上（未写完的字段见 emitError）
void BilibiliApi::emitLyricError(const QString &hash, QNetworkReply::NetworkError error)
{
    QVariantMap extra;
    extra.insert(QStringLiteral("translate"), QVariantList());
    extra.insert(QStringLiteral("hash"), hash);
    emitError(QStringLiteral("getLyricInfo"), error, extra);
}

void BilibiliApi::searchVideos(const QString &keyword, int tid, int page, const QString &action)
{
    if (keyword.trimmed().isEmpty()) {
        emitList(action, {});
        return;
    }
    QVariantMap params;
    params.insert(QStringLiteral("search_type"), QStringLiteral("video"));
    params.insert(QStringLiteral("keyword"), keyword);
    params.insert(QStringLiteral("page"), page);
    params.insert(QStringLiteral("tids"), tid);
    getSigned(QStringLiteral("/x/web-interface/wbi/search/type"), params,
              [this, action](const QJsonObject &j, QNetworkReply::NetworkError error) {
                  if (error != QNetworkReply::NoError) { // 失败：上报 error，不发空列表
                      emitError(action, error);
                      return;
                  }
                  QVariantList info;
                  for (const QJsonValue &v : j.value(QStringLiteral("data")).toObject()
                                                 .value(QStringLiteral("result")).toArray())
                      info << toSong(v.toObject());
                  emitList(action, info);
              });
}

void BilibiliApi::ranking(int rid, int page, int pageSize, const QString &action)
{
    get(kApi + QStringLiteral("/x/web-interface/ranking/v2?rid=%1&type=all").arg(rid),
        [this, page, pageSize, action](const QJsonObject &j, QNetworkReply::NetworkError error) {
            if (error != QNetworkReply::NoError) {
                emitError(action, error);
                return;
            }
            const QJsonArray arr = j.value(QStringLiteral("data")).toObject()
                                       .value(QStringLiteral("list")).toArray();
            const int offset = (qMax(page, 1) - 1) * qMax(pageSize, 1);
            QVariantList info;
            for (int i = offset; i < arr.size() && i < offset + pageSize; ++i)
                info << toSong(arr.at(i).toObject());
            emitList(action, info);
        });
}

void BilibiliApi::newVideos(int page, int pageSize, const QString &action)
{
    get(kApi + QStringLiteral("/x/web-interface/newlist?rid=3&type=0&pn=%1&ps=%2")
              .arg(qMax(page, 1))
              .arg(qMax(pageSize, 1)),
        [this, action](const QJsonObject &j, QNetworkReply::NetworkError error) {
            if (error != QNetworkReply::NoError) {
                emitError(action, error);
                return;
            }
            QVariantList info;
            for (const QJsonValue &v : j.value(QStringLiteral("data")).toObject()
                                           .value(QStringLiteral("archives")).toArray())
                info << toSong(v.toObject());
            emitList(action, info);
        });
}

void BilibiliApi::rankingUsers(const QString &action, int limit)
{
    get(kApi + QStringLiteral("/x/web-interface/ranking/v2?rid=3&type=all"),
        [this, action, limit](const QJsonObject &j, QNetworkReply::NetworkError error) {
            if (error != QNetworkReply::NoError) {
                emitError(action, error);
                return;
            }
            QVariantList info;
            QSet<QString> seen;
            for (const QJsonValue &v : j.value(QStringLiteral("data")).toObject()
                                           .value(QStringLiteral("list")).toArray()) {
                const QJsonObject o = v.toObject().value(QStringLiteral("owner")).toObject();
                const QString mid = o.value(QStringLiteral("mid")).toVariant().toString();
                if (mid.isEmpty() || seen.contains(mid))
                    continue;
                seen.insert(mid);
                m_upNames.insert(mid, o.value(QStringLiteral("name")).toString());
                info << ApiCommon::song(o.value(QStringLiteral("name")).toString(), QString(),
                                        httpsUrl(o.value(QStringLiteral("face")).toString()),
                                        mid);
                if (limit > 0 && info.size() >= limit)
                    break;
            }
            emitList(action, info);
        });
}

// 搜索（type: 0 歌曲；B 站没有歌单/专辑/歌词实体，其余类型返回空）
void BilibiliApi::searchSongs(const QString &keyword, int type, int page, int pageSize)
{
    Q_UNUSED(pageSize); // B 站搜索每页固定 20 条
    if (type != 0) {
        emitList(QStringLiteral("searchSongs"), {});
        return;
    }
    searchVideos(keyword, 3, page, QStringLiteral("searchSongs")); // tid 3 = 音乐区
}

// 分类（音乐分区）
void BilibiliApi::getPlaylistMenu(int type)
{
    Q_UNUSED(type);
    QVariantList info;
    for (const Region &r : kRegions) {
        const QString name = QString::fromUtf8(r.name);
        info << QVariantMap{
            {QStringLiteral("title"), name},
            {QStringLiteral("id"), name},
            {QStringLiteral("category"), name},
        };
    }
    emitList(QStringLiteral("getPlaylistMenu"), info);
}

void BilibiliApi::getMenuInfo(const QString &id)
{
    QVariantMap data;
    data.insert(QStringLiteral("special_tag_id"), id);
    data.insert(QStringLiteral("id"), id);
    data.insert(QStringLiteral("name"), id);
    emit resultReady(QStringLiteral("getMenuInfo"), data, Source);
}

void BilibiliApi::getMusicPlaylists(const QString &tagid, int page, int pageSize)
{
    Q_UNUSED(pageSize);
    searchVideos(tagid, 3, page, QStringLiteral("getMusicPlaylists"));
}

// 一条稿件即一首歌：直接按稿件 id 返回单曲
void BilibiliApi::getPlaylistSongs(const QString &listid, int page, int pageSize)
{
    Q_UNUSED(page);
    Q_UNUSED(pageSize);
    if (listid.isEmpty()) {
        emitList(QStringLiteral("getPlaylistSongs"), {});
        return;
    }
    get(kApi + QStringLiteral("/x/web-interface/view?bvid=") + listid,
        [this](const QJsonObject &j, QNetworkReply::NetworkError error) {
            if (error != QNetworkReply::NoError) {
                emitError(QStringLiteral("getPlaylistSongs"), error);
                return;
            }
            const QJsonObject d = j.value(QStringLiteral("data")).toObject();
            QVariantList info;
            if (!d.isEmpty())
                info << toSong(d);
            emitList(QStringLiteral("getPlaylistSongs"), info);
        });
}

void BilibiliApi::getRecommendSongs(int page, int pageSize)
{
    ranking(3, page, pageSize, QStringLiteral("getRecommendSongs"));
}

void BilibiliApi::getHotPlaylistMenu(int type)
{
    getPlaylistMenu(type);
}

void BilibiliApi::getHotPlaylists(int page, int pageSize)
{
    ranking(3, page, pageSize, QStringLiteral("getHotPlaylists"));
}

void BilibiliApi::getNewSongs(int type, int page, int pageSize)
{
    Q_UNUSED(type);
    newVideos(page, pageSize, QStringLiteral("getNewSongs"));
}

void BilibiliApi::getAllToplist()
{
    QVariantList info;
    for (const Region &r : kRegions) {
        const QString name = QString::fromUtf8(r.name);
        info << ApiCommon::song(name + QStringLiteral("榜"), QStringLiteral("哔哩哔哩"),
                                QString(), QString::number(r.tid), 0, name);
    }
    emitList(QStringLiteral("getAllToplist"), info);
}

void BilibiliApi::getMusicToplist(int page, int pageSize, int rankid)
{
    const QString name = regionName(rankid);
    if (name.isEmpty() || rankid == kRegions[0].tid) // 音乐区总榜走排行接口
        ranking(3, page, pageSize, QStringLiteral("getMusicToplist"));
    else
        searchVideos(name, rankid, page, QStringLiteral("getMusicToplist"));
}

void BilibiliApi::getHotSingers(int page, int pageSize)
{
    Q_UNUSED(page);
    rankingUsers(QStringLiteral("getHotSingers"), pageSize);
}

void BilibiliApi::getSingerCategory(int area, int page, int pageSize)
{
    Q_UNUSED(area);
    Q_UNUSED(page);
    rankingUsers(QStringLiteral("getSingerCategory"), pageSize);
}

// 歌手 = UP 主：按昵称搜索其音乐区投稿，再按 mid 过滤
void BilibiliApi::getSingerSongs(const QString &singerid, int page, int pageSize)
{
    const QString name = m_upNames.value(singerid);
    if (name.isEmpty()) {
        emitList(QStringLiteral("getSingerSongs"), {});
        return;
    }
    QVariantMap params;
    params.insert(QStringLiteral("search_type"), QStringLiteral("video"));
    params.insert(QStringLiteral("keyword"), name);
    params.insert(QStringLiteral("page"), page);
    params.insert(QStringLiteral("tids"), 3);
    // L2: 该搜索接口支持 page_size（实测 50 → 返回 50 条；≥60 会被服务端以 -400 拒绝，
    // 所以统一夹到 [1,50]），与 newVideos 的 ps 参数等价，避免调用方的页大小被静默忽略。
    params.insert(QStringLiteral("page_size"), qBound(1, pageSize, 50));
    getSigned(QStringLiteral("/x/web-interface/wbi/search/type"), params,
              [this, singerid](const QJsonObject &j, QNetworkReply::NetworkError error) {
                  if (error != QNetworkReply::NoError) {
                      emitError(QStringLiteral("getSingerSongs"), error);
                      return;
                  }
                  QVariantList info;
                  for (const QJsonValue &v : j.value(QStringLiteral("data")).toObject()
                                                 .value(QStringLiteral("result")).toArray()) {
                      const QJsonObject o = v.toObject();
                      if (o.value(QStringLiteral("mid")).toVariant().toString() != singerid)
                          continue;
                      info << toSong(o);
                  }
                  emitList(QStringLiteral("getSingerSongs"), info);
              });
}

// 播放信息：稿件详情 → DASH 音频流（type: 0 播放 / 1 下载）
void BilibiliApi::getMusicInfo(const QString &hash, int type)
{
    if (hash.isEmpty()) {
        QVariantMap data;
        data.insert(QStringLiteral("hash"), hash);
        data.insert(QStringLiteral("type"), type);
        data.insert(QStringLiteral("errReason"), QStringLiteral("unavailable"));
        emit resultReady(QStringLiteral("getMusicInfo"), data, Source);
        return;
    }

    get(kApi + QStringLiteral("/x/web-interface/view?bvid=") + hash,
        [this, hash, type](const QJsonObject &j, QNetworkReply::NetworkError error) {
            if (error != QNetworkReply::NoError) {
                qWarning() << "[bilibili] 稿件详情请求失败:" << error << hash;
                QVariantMap extra;
                extra.insert(QStringLiteral("hash"), hash);
                extra.insert(QStringLiteral("type"), type);
                extra.insert(QStringLiteral("errReason"), QStringLiteral("unavailable"));
                emitError(QStringLiteral("getMusicInfo"), error, extra);
                return;
            }
            const QJsonObject d = j.value(QStringLiteral("data")).toObject();
            const QString cid = d.value(QStringLiteral("cid")).toVariant().toString();
            if (cid.isEmpty()) { // 请求成功但没有 cid：属于“合法空数据”，不带 error
                QVariantMap data;
                data.insert(QStringLiteral("hash"), hash);
                data.insert(QStringLiteral("type"), type);
                data.insert(QStringLiteral("errReason"), QStringLiteral("unavailable"));
                emit resultReady(QStringLiteral("getMusicInfo"), data, Source);
                return;
            }

            get(kApi + QStringLiteral("/x/player/playurl?bvid=%1&cid=%2&fnval=16&fourk=1")
                          .arg(hash, cid),
                [this, hash, type, d](const QJsonObject &pj, QNetworkReply::NetworkError error2) {
                    if (error2 != QNetworkReply::NoError) {
                        qWarning() << "[bilibili] 播放地址请求失败:" << error2 << hash;
                        QVariantMap extra;
                        extra.insert(QStringLiteral("hash"), hash);
                        extra.insert(QStringLiteral("type"), type);
                        extra.insert(QStringLiteral("errReason"), QStringLiteral("unavailable"));
                        emitError(QStringLiteral("getMusicInfo"), error2, extra);
                        return;
                    }
                    const QJsonObject pd = pj.value(QStringLiteral("data")).toObject();
                    const QJsonArray audios = pd.value(QStringLiteral("dash")).toObject()
                                                  .value(QStringLiteral("audio")).toArray();
                    QString url;
                    if (!audios.isEmpty()) {
                        const QJsonObject best = pickAudio(audios, m_quality);
                        url = httpsUrl(best.value(QStringLiteral("baseUrl")).toString());
                        if (url.isEmpty())
                            url = httpsUrl(best.value(QStringLiteral("base_url")).toString());
                    }
                    bool durlFallback = false;
                    if (url.isEmpty()) { // 少数稿件只有 durl（音视频合流）
                        const QJsonArray durl = pd.value(QStringLiteral("durl")).toArray();
                        if (!durl.isEmpty()) {
                            url = httpsUrl(durl.first().toObject()
                                               .value(QStringLiteral("url")).toString());
                            durlFallback = !url.isEmpty();
                        }
                    }

                    const QJsonObject owner = d.value(QStringLiteral("owner")).toObject();
                    const QString title = cleanTitle(d.value(QStringLiteral("title")).toString());
                    QVariantMap data;
                    data.insert(QStringLiteral("backup_url"), url);
                    data.insert(QStringLiteral("url"), url);
                    data.insert(QStringLiteral("songName"), title);
                    data.insert(QStringLiteral("author_name"),
                                owner.value(QStringLiteral("name")).toString());
                    data.insert(QStringLiteral("singer_img"),
                                httpsUrl(owner.value(QStringLiteral("face")).toString()));
                    data.insert(QStringLiteral("album_img"),
                                httpsUrl(d.value(QStringLiteral("pic")).toString()));
                    data.insert(QStringLiteral("timeLength"),
                                d.value(QStringLiteral("duration")).toInt());
                    // L1: DASH 音频流固定 m4a；durl 回退是合流容器（可能 mp4/flv），
                    // 扩展名按 URL 推断，推断不出时退回默认值，避免扩展名与实际容器不符。
                    data.insert(QStringLiteral("fileName"),
                                title + QLatin1Char('.') + mediaSuffix(url, durlFallback));
                    data.insert(QStringLiteral("hash"), hash);
                    data.insert(QStringLiteral("type"), type);
                    if (url.isEmpty())
                        data.insert(QStringLiteral("errReason"),
                                    QStringLiteral("unavailable"));
                    emit resultReady(QStringLiteral("getMusicInfo"), data, Source);
                });
        });
}

// 歌词：取稿件 CC 字幕；没有字幕时返回空，由 MusicApiService 走在线搜词链路补全
void BilibiliApi::getLyricInfo(const QString &hash, int duration)
{
    Q_UNUSED(duration);
    if (hash.isEmpty()) {
        emitLyrics(hash, {});
        return;
    }
    get(kApi + QStringLiteral("/x/web-interface/view?bvid=") + hash,
        [this, hash](const QJsonObject &j, QNetworkReply::NetworkError error) {
            if (error != QNetworkReply::NoError) {
                qWarning() << "[bilibili] 歌词稿件详情请求失败:" << error << hash;
                emitLyricError(hash, error);
                return;
            }
            const QJsonObject d = j.value(QStringLiteral("data")).toObject();
            const QString cid = d.value(QStringLiteral("cid")).toVariant().toString();
            if (cid.isEmpty()) { // 请求成功但没有 cid：合法空数据，交给上层补词
                emitLyrics(hash, {});
                return;
            }

            get(kApi + QStringLiteral("/x/player/v2?bvid=%1&cid=%2").arg(hash, cid),
                [this, hash](const QJsonObject &pj, QNetworkReply::NetworkError error2) {
                    if (error2 != QNetworkReply::NoError) {
                        qWarning() << "[bilibili] 字幕信息请求失败:" << error2 << hash;
                        emitLyricError(hash, error2);
                        return;
                    }
                    const QJsonArray subs = pj.value(QStringLiteral("data")).toObject()
                                                .value(QStringLiteral("subtitle")).toObject()
                                                .value(QStringLiteral("subtitles")).toArray();
                    QString subUrl;
                    for (const QJsonValue &v : subs) {
                        const QString u = v.toObject()
                                              .value(QStringLiteral("subtitle_url")).toString();
                        if (!u.isEmpty()) {
                            subUrl = httpsUrl(u);
                            break;
                        }
                    }
                    if (subUrl.isEmpty()) { // 稿件确实没有字幕：合法空数据
                        emitLyrics(hash, {});
                        return;
                    }
                    get(subUrl, [this, hash](const QJsonObject &sj,
                                             QNetworkReply::NetworkError error3) {
                        if (error3 != QNetworkReply::NoError) {
                            qWarning() << "[bilibili] 字幕文件请求失败:" << error3 << hash;
                            emitLyricError(hash, error3);
                            return;
                        }
                        QVariantList lyrics;
                        for (const QJsonValue &v : sj.value(QStringLiteral("body")).toArray()) {
                            const QJsonObject l = v.toObject();
                            const QString text = l.value(QStringLiteral("content"))
                                                     .toString().trimmed();
                            if (text.isEmpty())
                                continue;
                            lyrics << QVariantMap{
                                {QStringLiteral("time"),
                                 int(l.value(QStringLiteral("from")).toDouble() * 1000)},
                                {QStringLiteral("text"), text},
                                {QStringLiteral("info"), QVariant()},
                            };
                        }
                        emitLyrics(hash, lyrics);
                    });
                });
        });
}

// 私人漫游 → 音乐区排行；私人雷达 → 音乐区最新投稿
void BilibiliApi::getPersonalFm(int page, int pageSize)
{
    ranking(3, page, pageSize, QStringLiteral("getPersonalFm"));
}

void BilibiliApi::getPersonalRadar(int page, int pageSize)
{
    newVideos(page, pageSize, QStringLiteral("getPersonalRadar"));
}
