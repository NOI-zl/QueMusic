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
    // 统一结果协议：data 为 { info: [...] }（列表）或单条信息 map
    void resultReady(const QString &action, const QVariant &data, int source);

private:
    using Callback = std::function<void(const QJsonObject &)>;
    using Task = std::function<void()>;

    void get(const QString &url, const Callback &cb);                       // GET + JSON 回调
    void getSigned(const QString &path, QVariantMap params, const Callback &cb); // WBI 签名 GET
    void ensureKeys(const Task &then);                                      // 懒加载 WBI 密钥与 buvid
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
    QList<Task> m_keyWaiters;
    QHash<QString, QString> m_upNames; // UP 主 mid → 昵称（歌手歌曲检索需要）
};

#endif // BILIBILIAPI_H
