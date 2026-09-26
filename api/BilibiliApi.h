// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 QueMusic Contributors
//
// BilibiliApi — 哔哩哔哩音乐在线接口（源码 2）。
// B 站官方「音乐/音频」平台已下线，音乐内容载体为音乐区稿件：
// 一条稿件即一首歌（hash 为 bvid），播放地址取 DASH 音频流，
// 歌词取稿件字幕（CC）——没有字幕时由 MusicApiService 复用在线搜词链路补全。
// 搜索类接口需要 WBI 签名 + buvid 指纹（自动获取并缓存）。
#ifndef BILIBILIAPI_H
#define BILIBILIAPI_H

#include <QHash>
#include <QList>
#include <QNetworkReply> // Callback / 失败上报需要 QNetworkReply::NetworkError 枚举
#include <QObject>
#include <QString>
#include <QVariant>
#include <QVariantMap>
#include <QStringList>
#include <functional>

class QJsonObject;
class QNetworkAccessManager;

class BilibiliApi : public QObject
{
    Q_OBJECT
public:
    static constexpr int Source = 2; // 平台编号（0 酷狗 / 1 网易云 / 2 哔哩哔哩）

    explicit BilibiliApi(QObject *parent = nullptr);

    // 音质（0 标准 / 1 高清 / 2+ 无损），映射到 DASH 音频 id 30216 / 30280 / 30251
    void setQuality(int q) { m_quality = q; }
    void setCookie(const QString &cookie) { m_loginCookie = cookie; }

    // 与 QML 侧 action 一一对应的方法
    void searchSongs(const QString &keyword, int type, int page, int pageSize);
    void getPlaylistMenu(int type);
    void getMenuInfo(const QString &id);
    void getMusicPlaylists(const QString &tagid, int page, int pageSize);
    void getPlaylistSongs(const QString &listid, int page, int pageSize);
    void getRecommendSongs(int page, int pageSize);
    void getHotPlaylistMenu(int type);
    void getHotPlaylists(int page, int pageSize);
    void getNewSongs(int type, int page, int pageSize);
    void getAllToplist();
    void getMusicToplist(int page, int pageSize, int rankid);
    void getHotSingers(int page, int pageSize);
    void getSingerCategory(int area, int page, int pageSize);
    void getSingerSongs(const QString &singerid, int page, int pageSize);
    void getMusicInfo(const QString &hash, int type);
    void getLyricInfo(const QString &hash, int duration);
    void getPersonalFm(int page, int pageSize);
    void getPersonalRadar(int page, int pageSize);

signals:
    // 统一结果协议：data 为 { info: [...] }（列表）或单条信息 map；
    // 请求失败时 data 里额外带 "error"（int，QNetworkReply::NetworkError 值），且 info 为空，
    // 上层据此区分“请求失败”与“接口正常返回空数据”。
    void resultReady(const QString &action, const QVariant &data, int source);

private:
    // 回调携带明确的结果状态：只有 HTTP 2xx 且 JSON 可解析时才是 NoError，
    // 其余情况 obj 为空对象、error 为真实失败原因（解析失败 → ProtocolFailure）
    using Callback = std::function<void(const QJsonObject &, QNetworkReply::NetworkError)>;
    using Task = std::function<void()>;
    // 密钥链等待者：参数为密钥链结果，NoError 表示成功
    using KeyWaiter = std::function<void(QNetworkReply::NetworkError)>;

    void get(const QString &url, const Callback &cb);                       // GET + JSON 回调
    void getSigned(const QString &path, QVariantMap params, const Callback &cb); // WBI 签名 GET
    // 懒加载 WBI 密钥与 buvid：成功 → 执行 then；失败 → 不执行 then，改执行 onFail（可为空）。
    // 语义：失败时不会“静默抽干”等待队列——已排队的任务一律丢弃，由 onFail 把 error 上报上层，
    // 否则上层既拿不到结果、loadState 也无法复位。
    void ensureKeys(const Task &then, const KeyWaiter &onFail = KeyWaiter());
    void emitError(const QString &action, QNetworkReply::NetworkError error,
                   const QVariantMap &extra = QVariantMap()); // 失败上报（data 带 error 字段）
    void emitLyricError(const QString &hash, QNetworkReply::NetworkError error); // 歌词失败上报
    void searchVideos(const QString &keyword, int tid, int page, const QString &action);
    void ranking(int rid, int page, int pageSize, const QString &action);   // rid=3 音乐区排行
    void newVideos(int page, int pageSize, const QString &action);          // 音乐区最新投稿
    void rankingUsers(const QString &action, int limit);                    // 由排行稿件的 UP 主去重成歌手
    void emitList(const QString &action, const QVariantList &info);
    void emitLyrics(const QString &hash, const QVariantList &lyrics);
    QVariantMap toSong(const QJsonObject &v); // 稿件（搜索/排行/详情）→ 统一歌曲字段

    static QString mixinKey(const QString &imgKey, const QString &subKey);

    QNetworkAccessManager *m_nam = nullptr;
    QString m_mixinKey;
    QString m_buvid3;
    QString m_buvid4;
    QString m_loginCookie;
    int m_quality = 1;
    bool m_keyRequesting = false;
    QList<KeyWaiter> m_keyWaiters;
    QHash<QString, QString> m_upNames; // UP 主 mid → 昵称（歌手歌曲检索需要）
};

#endif // BILIBILIAPI_H
