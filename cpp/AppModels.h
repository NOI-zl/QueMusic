// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2025-2026 QueMusic Contributors
//
// 面向 QML 的数据模型单例。
//
// 这些模型原先用 QQmlContext::setContextProperty() 暴露，上下文属性无法被 qmlcachegen 在
// 编译期解析，绑定只能退回解释执行；拆成独立单例后 QML 按类型名访问（如 FavoriteSongs.count）
// 才能编译成 C++。不能改成"一个单例持有多个模型属性"：qmlcachegen 会拒绝继续属性查找。
//
// ⚠️ 构造函数【绝不能】带默认参数（务必保留注释）：qqmlprivate.h 的
// singletonConstructionMode() 中 is_default_constructible 优先于 HasSingletonFactory，
// 写成 X(QObject *parent = nullptr) 会走默认构造分支、create() 永不执行，过滤类型不会设置，
// 表现为各收藏列表把全部数据列出来。显式去掉默认参数即可强制走 create() 分支。
#ifndef APPMODELS_H
#define APPMODELS_H

#include <QJSEngine>
#include <QQmlEngine>

#include "Favorites.h"
#include "FolderModel.h"

#define QUEMUSIC_DECLARE_MODEL_SINGLETON(CLASS, BASE, FILTER) \
    class CLASS : public BASE                                    \
    {                                                            \
        Q_OBJECT                                                 \
        QML_ELEMENT                                              \
        QML_SINGLETON                                            \
    public:                                                      \
        explicit CLASS(QObject *parent) : BASE(parent) {}         \
        static CLASS *create(QQmlEngine *qmlEngine, QJSEngine *)  \
        {                                                        \
            CLASS *model = new CLASS(qmlEngine);                 \
            model->setFilterType(QStringLiteral(FILTER));        \
            return model;                                        \
        }                                                        \
    }

// 我的歌单 / 本地音乐（过滤类型即模型自身的语义）
QUEMUSIC_DECLARE_MODEL_SINGLETON(MyFolders, FolderModel, "my");
QUEMUSIC_DECLARE_MODEL_SINGLETON(LocalFolders, FolderModel, "local");

// 歌单内的歌曲列表（该模型不使用过滤类型）
class Songs : public SongModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
public:
    explicit Songs(QObject *parent) : SongModel(parent) {}
    static Songs *create(QQmlEngine *qmlEngine, QJSEngine *)
    {
        return new Songs(qmlEngine);
    }
};

// 收藏：歌曲 / 歌单 / 歌手
QUEMUSIC_DECLARE_MODEL_SINGLETON(FavoriteSongs, FavoritesModel, "song");
QUEMUSIC_DECLARE_MODEL_SINGLETON(FavoritePlaylists, FavoritesModel, "playlist");
QUEMUSIC_DECLARE_MODEL_SINGLETON(FavoriteArtists, FavoritesModel, "artist");

#undef QUEMUSIC_DECLARE_MODEL_SINGLETON

#endif // APPMODELS_H
