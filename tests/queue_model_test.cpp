// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 QueMusic Contributors
//
// H4 回归测试：QueueModel 的 path → 行号缓存必须在每种修改操作之后与 m_items 行号一致。
// 不变量：对任意行号 i，若 m_items[i].path 非空，则 indexOfPath(m_items[i].path) 必须等于
// 该 path 首次出现的真实行号；查不到时返回 -1。
//
// 整个测试过程（每个测试函数）都挂着一个 QAbstractItemModelTester：
// 模型一旦违反 QAbstractItemModel 的 insert/remove/move 契约，当前测试立刻判为失败。

#include <QtTest>
#include <QtTest/QAbstractItemModelTester>

#include <QRegularExpression>

#include "QueueModel.h"

namespace {

QString musicName(int i)
{
    return QStringLiteral("歌%1").arg(i);
}

QString musicPath(int i)
{
    return QStringLiteral("/music/%1.mp3").arg(i);
}

} // namespace

class QueueModelTest : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void insertAtHeadShiftsFollowingIndices();
    void insertInMiddleShiftsFollowingIndices();
    void moveForwardKeepsIndicesInSync();
    void moveBackwardKeepsIndicesInSync();
    void removeThenQueryAgain();
    void clearThenQueryAgain();
    void indexOfNameUnaffected();
    void sequentialMutationsKeepCacheInSync();
    void duplicatePathResolvesToFirstRow();

private:
    static QVariantMap makeTrack(const QString &name, const QString &path);
    void fillQueue(int size);
    QStringList rowNames() const;
    void verifyIndexCacheMatchesRows() const;

    QueueModel *m_model = nullptr;
    QAbstractItemModelTester *m_tester = nullptr;
};

QVariantMap QueueModelTest::makeTrack(const QString &name, const QString &path)
{
    return {
        { QStringLiteral("name"), name },
        { QStringLiteral("path"), path },
        { QStringLiteral("songer"), QStringLiteral("测试歌手") },
        { QStringLiteral("source"), 0 }
    };
}

// 每个测试函数用全新的模型，tester 随之挂在该模型上（父对象为模型，保证先于模型析构）。
void QueueModelTest::init()
{
    // Qt 6 的 QAbstractItemModelTester 没有叫 FailOnWarning 的枚举，等价写法是
    // FailureReportingMode::Warning（模型违反契约时以 qt.modeltest 类别发 qWarning）
    // 再配合 failOnWarning()：命中的警告会把当前测试直接判为失败。
    // 这里只匹配 tester 自己的 "FAIL! ..." 前缀（见 qtbase 的 qabstractitemmodeltester.cpp），
    // 因为本测试按项目约定只链接 Core/Qml/Test、不链接 QtGui，而模型 tester 检查 Gui 角色
    // （QPixmap / QFont 等）时会额外发一条与本模型无关的内部警告：
    // "Trying to construct an instance of an invalid type, type id: 4097"（4097 = QPixmap）。
    QTest::failOnWarning(QRegularExpression(QStringLiteral("^FAIL! ")));

    m_model = new QueueModel;
    m_tester = new QAbstractItemModelTester(
        m_model, QAbstractItemModelTester::FailureReportingMode::Warning, m_model);
}

void QueueModelTest::cleanup()
{
    delete m_model; // tester 是模型的子对象，随之析构
    m_model = nullptr;
    m_tester = nullptr;
}

void QueueModelTest::fillQueue(int size)
{
    for (int i = 0; i < size; ++i)
        m_model->append(makeTrack(musicName(i), musicPath(i)));
}

QStringList QueueModelTest::rowNames() const
{
    QStringList names;
    for (int row = 0; row < m_model->count(); ++row)
        names << m_model->data(m_model->index(row, 0), QueueModel::NameRole).toString();
    return names;
}

void QueueModelTest::verifyIndexCacheMatchesRows() const
{
    // 逐行核对：每一行的 path 都要查得到，且查到的必须是该 path 首次出现的真实行号。
    QHash<QString, int> firstRowOfPath;
    for (int row = 0; row < m_model->count(); ++row) {
        const QModelIndex index = m_model->index(row, 0);
        const QString path = m_model->data(index, QueueModel::PathRole).toString();
        const QString name = m_model->data(index, QueueModel::NameRole).toString();
        QVERIFY2(!path.isEmpty(), "测试数据不应出现空 path");

        const auto it = firstRowOfPath.constFind(path);
        if (it == firstRowOfPath.constEnd()) {
            firstRowOfPath.insert(path, row);
            QCOMPARE(m_model->indexOfPath(path), row);
        } else {
            QCOMPARE(m_model->indexOfPath(path), it.value());
        }
        // indexOfName 是线性查找，不受缓存影响，任何时刻都应等于真实行号。
        QCOMPARE(m_model->indexOfName(name), row);
    }
    // 不在队列里的 path 必须返回 -1。
    QCOMPARE(m_model->indexOfPath(QStringLiteral("/music/absent.mp3")), -1);
}

// 头部插入：原有各行行号整体 +1，缓存必须跟着平移。
void QueueModelTest::insertAtHeadShiftsFollowingIndices()
{
    fillQueue(3); // 行 0/1/2 = 歌0/歌1/歌2
    QCOMPARE(m_model->indexOfPath(musicPath(1)), 1);

    m_model->insert(0, makeTrack(QStringLiteral("插入头部"), QStringLiteral("/music/head.mp3")));

    QCOMPARE(rowNames(), QStringList({ QStringLiteral("插入头部"), musicName(0), musicName(1), musicName(2) }));
    QCOMPARE(m_model->indexOfPath(QStringLiteral("/music/head.mp3")), 0);
    QCOMPARE(m_model->indexOfPath(musicPath(0)), 1);
    QCOMPARE(m_model->indexOfPath(musicPath(1)), 2);
    QCOMPARE(m_model->indexOfPath(musicPath(2)), 3);
    verifyIndexCacheMatchesRows();
}

// 中间插入：index 之后的行行号整体 +1，尾部追加则不改动已有行号。
void QueueModelTest::insertInMiddleShiftsFollowingIndices()
{
    fillQueue(4); // 行 0..3 = 歌0..歌3

    m_model->insert(2, makeTrack(QStringLiteral("插入中间"), QStringLiteral("/music/mid.mp3")));

    QCOMPARE(rowNames(), QStringList({ musicName(0), musicName(1), QStringLiteral("插入中间"),
                                       musicName(2), musicName(3) }));
    QCOMPARE(m_model->indexOfPath(musicPath(0)), 0);
    QCOMPARE(m_model->indexOfPath(musicPath(1)), 1);
    QCOMPARE(m_model->indexOfPath(QStringLiteral("/music/mid.mp3")), 2); // 行 2 之后的旧行后移
    QCOMPARE(m_model->indexOfPath(musicPath(2)), 3);
    QCOMPARE(m_model->indexOfPath(musicPath(3)), 4);
    verifyIndexCacheMatchesRows();

    m_model->append(makeTrack(QStringLiteral("追加尾部"), QStringLiteral("/music/tail.mp3")));

    QCOMPARE(rowNames(), QStringList({ musicName(0), musicName(1), QStringLiteral("插入中间"),
                                       musicName(2), musicName(3), QStringLiteral("追加尾部") }));
    QCOMPARE(m_model->indexOfPath(QStringLiteral("/music/tail.mp3")), 5);
    QCOMPARE(m_model->indexOfPath(musicPath(2)), 3); // 尾部追加不影响已有行号
    verifyIndexCacheMatchesRows();
}

// 前移：move 之后每个剩余 path 的 indexOfPath 都必须等于其真实行号。
void QueueModelTest::moveForwardKeepsIndicesInSync()
{
    fillQueue(5); // 行 0..4 = 歌0..歌4

    // 相邻后移，等价于 PlayList.qml 里的 playListModel.move(index, cur + 1, 1)。
    m_model->move(1, 2, 1);

    QCOMPARE(rowNames(), QStringList({ musicName(0), musicName(2), musicName(1),
                                       musicName(3), musicName(4) }));
    QCOMPARE(m_model->indexOfPath(musicPath(0)), 0);
    QCOMPARE(m_model->indexOfPath(musicPath(2)), 1);
    QCOMPARE(m_model->indexOfPath(musicPath(1)), 2); // 被移动的行落在 to 行
    QCOMPARE(m_model->indexOfPath(musicPath(3)), 3);
    QCOMPARE(m_model->indexOfPath(musicPath(4)), 4);
    verifyIndexCacheMatchesRows();

    // 跨多行前移：歌0 从行 0 移到行 4，中间所有行的行号都变了。
    m_model->move(0, 4, 1);

    QCOMPARE(rowNames(), QStringList({ musicName(2), musicName(1), musicName(3),
                                       musicName(4), musicName(0) }));
    QCOMPARE(m_model->indexOfPath(musicPath(0)), 4);
    QCOMPARE(m_model->indexOfPath(musicPath(2)), 0);
    verifyIndexCacheMatchesRows();
}

// 后移：与前移共用同一套整体重建策略。
void QueueModelTest::moveBackwardKeepsIndicesInSync()
{
    fillQueue(5); // 行 0..4 = 歌0..歌4

    m_model->move(4, 1, 1); // 歌4 从行 4 移到行 1

    QCOMPARE(rowNames(), QStringList({ musicName(0), musicName(4), musicName(1),
                                       musicName(2), musicName(3) }));
    QCOMPARE(m_model->indexOfPath(musicPath(0)), 0);
    QCOMPARE(m_model->indexOfPath(musicPath(4)), 1);
    QCOMPARE(m_model->indexOfPath(musicPath(1)), 2);
    QCOMPARE(m_model->indexOfPath(musicPath(2)), 3);
    QCOMPARE(m_model->indexOfPath(musicPath(3)), 4);
    verifyIndexCacheMatchesRows();

    m_model->move(3, 0, 1); // 歌2 从行 3 移到队首

    QCOMPARE(rowNames(), QStringList({ musicName(2), musicName(0), musicName(4),
                                       musicName(1), musicName(3) }));
    QCOMPARE(m_model->indexOfPath(musicPath(2)), 0);
    QCOMPARE(m_model->indexOfPath(musicPath(1)), 3);
    verifyIndexCacheMatchesRows();
}

// 删除之后再次查询：被删的 path 归 -1，剩余行号前移。
void QueueModelTest::removeThenQueryAgain()
{
    fillQueue(5);

    m_model->remove(1, 2); // 删掉 歌1、歌2

    QCOMPARE(rowNames(), QStringList({ musicName(0), musicName(3), musicName(4) }));
    QCOMPARE(m_model->indexOfPath(musicPath(0)), 0);
    QCOMPARE(m_model->indexOfPath(musicPath(3)), 1);
    QCOMPARE(m_model->indexOfPath(musicPath(4)), 2);
    QCOMPARE(m_model->indexOfPath(musicPath(1)), -1);
    QCOMPARE(m_model->indexOfPath(musicPath(2)), -1);
    verifyIndexCacheMatchesRows();

    m_model->remove(0); // 删除队首

    QCOMPARE(rowNames(), QStringList({ musicName(3), musicName(4) }));
    QCOMPARE(m_model->indexOfPath(musicPath(3)), 0);
    QCOMPARE(m_model->indexOfPath(musicPath(4)), 1);
    QCOMPARE(m_model->indexOfPath(musicPath(0)), -1);
    verifyIndexCacheMatchesRows();
}

// 清空之后再次查询：全部归 -1，重新入队后缓存重建。
void QueueModelTest::clearThenQueryAgain()
{
    fillQueue(3);

    m_model->clear();

    QCOMPARE(m_model->count(), 0);
    QCOMPARE(rowNames(), QStringList());
    QCOMPARE(m_model->indexOfPath(musicPath(0)), -1);
    QCOMPARE(m_model->indexOfPath(musicPath(1)), -1);
    QCOMPARE(m_model->indexOfPath(musicPath(2)), -1);
    verifyIndexCacheMatchesRows();

    m_model->clear(); // 空队列重复 clear 不应崩溃，缓存保持为空
    QCOMPARE(m_model->count(), 0);
    QCOMPARE(m_model->indexOfPath(musicPath(0)), -1);

    fillQueue(2); // 重新入队，行号从 0 开始
    QCOMPARE(m_model->indexOfPath(musicPath(0)), 0);
    QCOMPARE(m_model->indexOfPath(musicPath(1)), 1);
    verifyIndexCacheMatchesRows();
}

// indexOfName 走线性查找，插入/移动/删除都不应影响它。
void QueueModelTest::indexOfNameUnaffected()
{
    fillQueue(4); // 行 0..3 = 歌0..歌3

    m_model->insert(1, makeTrack(QStringLiteral("新歌"), QStringLiteral("/music/new.mp3")));
    m_model->move(3, 0, 1); // 歌2 移到队首

    QCOMPARE(rowNames(), QStringList({ musicName(2), musicName(0), QStringLiteral("新歌"),
                                       musicName(1), musicName(3) }));
    QCOMPARE(m_model->indexOfName(musicName(2)), 0);
    QCOMPARE(m_model->indexOfName(musicName(0)), 1);
    QCOMPARE(m_model->indexOfName(QStringLiteral("新歌")), 2);
    QCOMPARE(m_model->indexOfName(musicName(1)), 3);
    QCOMPARE(m_model->indexOfName(musicName(3)), 4);
    QCOMPARE(m_model->indexOfName(QStringLiteral("不存在")), -1);

    QCOMPARE(m_model->indexOfPath(musicPath(2)), 0);
    QCOMPARE(m_model->indexOfPath(QStringLiteral("/music/new.mp3")), 2);
    verifyIndexCacheMatchesRows();
}

// 连续混合修改（insert → move → move → insert → remove），每步之后整体校验。
void QueueModelTest::sequentialMutationsKeepCacheInSync()
{
    const QVariantMap insertA = makeTrack(QStringLiteral("插曲甲"), QStringLiteral("/music/ins-a.mp3"));
    const QVariantMap insertB = makeTrack(QStringLiteral("插曲乙"), QStringLiteral("/music/ins-b.mp3"));

    fillQueue(4); // [歌0, 歌1, 歌2, 歌3]

    m_model->insert(1, insertA); // [歌0, 插曲甲, 歌1, 歌2, 歌3]
    verifyIndexCacheMatchesRows();

    m_model->move(3, 0, 1); // [歌2, 歌0, 插曲甲, 歌1, 歌3]
    verifyIndexCacheMatchesRows();

    m_model->move(4, 1, 1); // [歌2, 歌3, 歌0, 插曲甲, 歌1]
    verifyIndexCacheMatchesRows();

    m_model->insert(2, insertB); // [歌2, 歌3, 插曲乙, 歌0, 插曲甲, 歌1]
    verifyIndexCacheMatchesRows();

    m_model->remove(0); // [歌3, 插曲乙, 歌0, 插曲甲, 歌1]
    verifyIndexCacheMatchesRows();

    QCOMPARE(rowNames(), QStringList({ musicName(3), QStringLiteral("插曲乙"), musicName(0),
                                       QStringLiteral("插曲甲"), musicName(1) }));
    QCOMPARE(m_model->indexOfPath(musicPath(3)), 0);
    QCOMPARE(m_model->indexOfPath(QStringLiteral("/music/ins-b.mp3")), 1);
    QCOMPARE(m_model->indexOfPath(musicPath(0)), 2);
    QCOMPARE(m_model->indexOfPath(QStringLiteral("/music/ins-a.mp3")), 3);
    QCOMPARE(m_model->indexOfPath(musicPath(1)), 4);
    QCOMPARE(m_model->indexOfPath(musicPath(2)), -1); // 歌2 已被最后的 remove(0) 删除
}

// 重复 path：缓存统一指向“首次出现”的行，与 indexOfName 的首次匹配语义一致。
void QueueModelTest::duplicatePathResolvesToFirstRow()
{
    const QString samePath = QStringLiteral("/music/same.mp3");
    m_model->append(makeTrack(QStringLiteral("甲"), samePath));
    m_model->append(makeTrack(QStringLiteral("乙"), QStringLiteral("/music/other.mp3")));
    m_model->append(makeTrack(QStringLiteral("丙"), samePath)); // 与甲重复

    QCOMPARE(m_model->indexOfPath(samePath), 0);
    QCOMPARE(m_model->indexOfPath(QStringLiteral("/music/other.mp3")), 1);
    verifyIndexCacheMatchesRows();

    // 头部插入同 path：新的行 0 成为首次出现。
    m_model->insert(0, makeTrack(QStringLiteral("丁"), samePath));
    QCOMPARE(rowNames(), QStringList({ QStringLiteral("丁"), QStringLiteral("甲"),
                                       QStringLiteral("乙"), QStringLiteral("丙") }));
    QCOMPARE(m_model->indexOfPath(samePath), 0);
    verifyIndexCacheMatchesRows();

    // 移动队首（首次出现的行）：缓存必须指向新的“首次出现”行。
    m_model->move(0, 2, 1); // [甲, 乙, 丁, 丙]：甲 仍是最靠前的同 path 行，缓存仍是 0
    QCOMPARE(rowNames(), QStringList({ QStringLiteral("甲"), QStringLiteral("乙"),
                                       QStringLiteral("丁"), QStringLiteral("丙") }));
    QCOMPARE(m_model->indexOfPath(samePath), 0);
    verifyIndexCacheMatchesRows();

    m_model->move(0, 2, 1); // [乙, 丁, 甲, 丙]：首次出现变成行 1 的 丁，缓存必须为 1
    QCOMPARE(rowNames(), QStringList({ QStringLiteral("乙"), QStringLiteral("丁"),
                                       QStringLiteral("甲"), QStringLiteral("丙") }));
    QCOMPARE(m_model->indexOfPath(samePath), 1);
    QCOMPARE(m_model->indexOfName(QStringLiteral("丁")), 1);
    verifyIndexCacheMatchesRows();
}

QTEST_MAIN(QueueModelTest)
#include "queue_model_test.moc"