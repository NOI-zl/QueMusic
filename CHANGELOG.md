# Changelog

本项目所有重要的变更都会记录在此文件中。

格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [Unreleased]

### ⚡ 优化
- **音频回调内零分配**：`applyPendingParams()` 原在参数变化时构造 `QList<qreal>`（拖 EQ 滑块即每次回调都分配
  内存）。新增 `AudioDsp::setEqGains(const double *, int)` 直读快照数组，音频线程不再触碰容器分配
- **部署体积 −19MB**：引擎只用 `QAudioSink` 输出、解码走自带 FFmpeg，Qt 的 `multimedia/ffmpegmediaplugin.dll`
  及其依赖的 61 系 FFmpeg 库（`avcodec-61`/`avformat-61`/`avutil-59`）永不被加载，已在 CMake 部署后自动删除
  （实测移走后音频测试 13 项全过、0 跳过）；`windowsmediaplugin`（音频设备后端）保留

### 🐞 修复
- **切歌闪退（0xC0000005 @ `Qt6Core!QMetaObject::indexOfProperty`）【根因已定位并修复】**：崩溃是按名字
  查属性时拿到空 `metaObject`，栈为 `Qt6Core!QMetaObject::indexOfProperty(NULL)` ←
  `Qt6Qml!AOTCompiledContext::initGetValueLookup`。**根因只有一行，在 `main.qml` 的 `startTrack()`**：
  `if (!e || e.path === undefined) return` —— `e` 是队列条目（非布尔值）却用 `!e` 取反，又用 `===`
  比较没有类型的 `undefined`，AOT 编译下这会让紧随其后的属性查找推导出空 `metaObject` 而直接崩
  （解释执行不走这条查找，所以关闭 AOT 就不崩）。改为 `if (e.path == undefined) return`
  （`==` 同样覆盖 null，且 AOT 安全）后，**AOT（qmlcachegen）保持开启也不再闪退，无需任何环境变量**。
  该行由 `Playback` 的 `playIndex` 信号 → `Connections.onPlayIndex` → `startTrack` 这条切歌链路触发，
  故表现为按「上一首/下一首」时闪退、播放列表点击正常
- **同类隐患写法（未在本轮触发，建议后续留意）**：`layout/PlayerMaxCenter.qml:375-378,661,663`、
  `FullCenterView.qml:108`、`components/QDrop.qml:19,60,139` 使用了 `X !== undefined` 判断；
  与上述根因同源，在 AOT 下同样有推导出空 `metaObject` 的风险，建议统一改为 `== undefined` 形式
- **切歌级联中的 SMTC 重入、`musicControlMin` 跨作用域引用**：两者都是真实缺陷，但**均非本次闪退的
  根因**（各自修复后崩溃仍复现）。已一并修正：`musicControlMin` 的跨文件引用统一走 `Playback` 单例；
  原生 SMTC 推送合并进 `pushSmtc()` 并由 `Qt.callLater` 延后，`onSourceChanged` 中的重复推送已清除
- **`Playback.qml` 切歌链路整理**：`next/previous` 不再各自调用 `notePlayed`，统一由 `goTo()` 记录
  （此前播放列表点击走的是 `goTo`、不记录，导致真随机的「最近播放」避让在两条路径上不一致）；
  链路本身保持 `next/previous → goTo → playIndex 信号 → Connections → startTrack` 不变
- **上一首/下一首闪退（自动跳歌死循环）**：`onErrorOccurred` 里任何一次播放失败都会自动跳下一首，
  且没有次数上限——队列里存在失效条目（文件被移走/在线源失效）时，会陷入「失败→跳→失败→跳」的
  高速循环，每轮都在重建音频输出，最终闪退。点播放列表能正常播是因为点的就是看得见、能播的那首，
  按队列顺序走的上一首/下一首才会撞上坏条目。现在连续失败 2 次即停止并提示，成功起播后清零
- **`startTrack` 的越界判断失效**：`QueueModel::get()` 越界返回空 `QVariantMap`，而空对象在 QML 中
  是真值，`if (!e) return` 拦不住，会带着 `undefined` 走在线分支。改为按字段判断 `e.path`
- **切歌闪退（0xC0000005）**：`reconfigureOutput()` 在 GUI 线程重建缓冲区时会 `resize`/`configure`
  音频线程正在写入的 `m_scratch` / `m_stretchIn` / 环形缓冲——旧内存被释放而音频线程仍持有指针，
  属于堆破坏。现在缓冲一律在输出线程内重建（与 `render()` 同线程，天然互斥），解码线程先停靠再重建；
  并且重建不再销毁供 sink 拉取的 QIODevice（Qt 音频后端可能在销毁后仍投递一次读取，导致空指针解引用）；
  `stateChanged` 回调增加 sender 校验，避免切歌时把旧 sink 的延迟信号误判为设备故障而触发重建。
  新增 `reconfigureWhilePlaying` 回归测试（播放中反复重建输出 + 切歌）
- **窗口缩放导致音频停顿/消失**：实测确认 Windows 上 `QAudioSink` 拉模式的 `readData` 回调跑在
  **GUI 线程**（探针实测 `render thread = "Qt mainThread", sameAsGui = true`），任何界面卡顿都会
  直接饿死音频输出。现在 sink 在专用输出线程（`QueMusicAudioOutput`）内创建，拉取定时器随之归属
  该线程，音频渲染彻底脱离 GUI（复测 `sameAsGui = false`）。新增回归测试锁死线程归属
- **切歌/上一首/下一首闪退**：`QAudioSink::stateChanged` 由音频线程发出，回调里直接改了 QML 状态并发
  `mediaStatusChanged`，导致 QML 处理器在音频线程上执行（崩溃定位：`Qt6Core.dll+0xb8b4c`，`0xc0000005`，
  三次现场偏移完全一致）。现在音频线程只置原子标志，状态变更统一回引擎线程处理
- **改变窗口宽度导致音频一顿一顿**：频谱通路用互斥量在音频回调与渲染线程之间传数据，窗口 resize 时渲染
  线程变慢会把音频线程卡在锁上造成欠载。改为无锁环形缓冲，音频回调全程不取锁
- 音高/倍速联动错误：重采样读指针压实缓冲时多减了一次基准偏移，导致读位置回退、时长被拉长约 2 倍

### 🔧 变更
- 音量与淡入淡出下沉到音频线程逐样本完成（`AudioDsp::processVolume`），不再由 QML 动画在 GUI 线程
  每帧调 `QAudioSink::setVolume`（那是 COM 调用，GUI 忙时会抖动）
- 切歌过渡：淡出在 C++ 完成，淡入在**新数据段真正出声的第一批样本**上开始推进；每次切段另有 12ms
  保护性淡入消除爆音
- 结束不再关闭输出设备（保持常开），下一首起播没有设备重开的空档
- 播放缓冲限制在 **10–200 ms**（0 = 自动 120 ms）；UI 滑杆同步收窄
- `AudioDsp` 新增旁路开关：总开关关闭时跳过均衡/声道/限幅，但音量与淡变仍生效

### ✨ 新增
- **音高调节**：±12 半音（一个八度），不影响播放速度与进度推进；与倍速可叠加
- 自测增加到 11 项，新增：切歌压力（连续换源 + seek + 自然结束）、变速换算方向、
  进度不回退、配置持久化、变调时长/频率校验（过零计数）

### 🐞 修复（前一轮）
- 开启音高补偿后崩溃：SOLA 相关搜索窗相对偏移算错，倍速 <1 时会读到输入缓冲之前的地址（加中心偏移 + 上下界夹取）
- 倍速方向反了：解码输出采样率与倍速写成正比，0.8x 实际按 1.2x 播放（改为反比）
- 播放进度不随倍速换算，且变速时进度按设备采样率推进导致刻度漂移
- 拖动进度条会先闪回旧位置：seek 落地前音频线程仍在发布上一数据段的位置（新增 pending-seek 抑制 + 解码线程 seek 节流）
- 拖动进度条时反复冲刷解码缓冲造成断续（节流至 ~11 次/秒，最后一次请求必定生效）
- `TimeStretch` 每跳一步都搬移整个输入缓冲（改为每次 push 回收一次）；缓冲无上限增长
- `QAudioSink` 用 `deleteLater` 释放，与紧随其后的 IO 设备销毁顺序不确定（改为同步销毁）

### 🔧 变更
- 升级到 Qt 6.10.3（Windows 主用 LLVM-MinGW 工具链）
- 第三方依赖（QWindowKit / TagLib / zlib）的导入统一收敛到 `cmake/external/`
- “设置-关于”中的 Qt 版本改为运行时动态获取，不再硬编码
- 移除调试遗留的 `QSG_INFO` 输出
- **音频后端重写**：不再使用 `QMediaPlayer` / `QAudioOutput`，改为 `cpp/audio/` 自研播放引擎
  - 解码：FFmpeg（libavformat/libavcodec/libswresample），支持全部常见音视频封装与音频编码
  - 管线：解码线程 → 无锁环形缓冲 → 设备输出回调（DSP 在音频线程内完成，低延迟）
  - 输出：`QAudioSink` 拉模式，优先 Float32，回退 Int16；支持指定输出设备
  - 频谱：由引擎后处理结果直接驱动 `GetWave`，不再依赖 `QAudioBufferOutput`

### ✨ 新增
- 音频 DSP：10 段参数均衡（±24 dB，含 10 组预设）、可调频段 Q 值、前级增益、自动余量、
  声道平衡、左右独立增益、单声道、立体声宽度、声道交换、ReplayGain（单曲/专辑 + 削波保护）、
  峰值限幅器、保持音高的 0.25×~4× 变速
- 音频处理总开关（一键直通）、输出采样率与输出缓冲可调（改后重开输出并保留播放位置）
- 设置页新增「均衡器与音频处理」面板：状态行显示编码/源采样率/位率/解码采样率/缓冲/DSP 指示
- 全部音频处理参数持久化到 `Options.ini`（写盘去抖 400ms，退出时兜底刷写）
- 新增 `quemusic_audio_pipeline_test` 离线自测：解码 / seek / DSP 边界 / SOLA 全量程变速 /
  倍速换算方向 / 进度不回退 / 配置持久化（9 项）
- 设置页（主题、界面、功能、播放、快捷键、插件、关于、Debug 八大模块）
- 桌面歌词 / 桌面部件
- 下载管理器（队列、进度、重试、已完成列表）
- 本地音乐库（我的文件夹 / 本地文件夹、导入、重命名、删除）

### 🎨 界面
- 全局单例样式系统（Style.qml），支持浅色/深色/跟随系统
- 自适应封面主色提取（ColorExtractor）
- 歌词逐字滚动 + 逐行弹簧动画 + 背景动态流体着色器

## [0.1.0] - 2026-08-01

### ✨ 新增
- Qt 6.9 / QML / RHI 跨平台框架
- 集成网易云、酷狗音乐 API（搜索、歌单、排行榜、新歌、歌词）
- QWindowKit 无边框窗口（毛玻璃 / 云母效果）
- 多平台支持：Windows / macOS / Linux

### 🧩 依赖
- Qt 6.9.3（Core, Gui, Qml, Quick, Network, Multimedia, Concurrent, Sql, ShaderTools）
- QWindowKit（Apache-2.0）
- pako.js（MIT）


### [0.2.5] - 2026-08-10

### 新增
- 全面将在线api转到c++提升性能，稳定性，为之后接入QCloudMusicApi以及QQ音乐做好准备
- 全面重做歌词界面，背景效果由AMLL Core移植，歌词组件抛弃ListView转用自定义排列，效果大更新
- 增加更多功能，在歌词界面，本地文件夹，设置调节功能，界面功能都有大量更新

### 其他
- 修复大量Bug，大量之前一直存在的一些小问题，部分优化性能
- 规范部分代码，规范协议

### [0.3.0] - 2026-08-16

- 全面接入QCloudMusicApi，网易云音乐接口已基本完毕，已支持登陆。
- UI全面翻新，优化UI布局，配色。
- 歌词界面性能大优化，优化。
- 新增快捷键功能，支持自定义快捷键以实现快速控制播放器。
- 还有许多许多的小更新以及功能补充，例如：播放列表全部删除按钮，关于页面更多按钮，新增补充一些设置项功能。
- 修复大量Bug，修复大量布局问题。

### [0.4.0] - 2026-08-23

### 功能
- 大幅完善在线功能：如私人漫游，分类下的歌单，排行榜，歌手。（重点）
- 歌词界面新增对唱歌词，并大幅优化访问开销，稳定性和性能大幅增长。
- 新增搜索历史记录。
- 新增桌面歌词功能。
- 新增点击歌曲名和歌手使用搜索。

### 修复
- Linux版本修复设置问题（实际已在Beta0.3.1的新增加的补丁修复）。
- 优化部分ui界面细节。
- 修复本地歌曲播放的部分问题，如背景流体不跟随封面更改。
- 修复右键菜单和设置部分卡片主题颜色问题（特别是深色）。
- 还修复一些小bug，详情可自己体验。

### [0.5.0] - 2026-9-16

### 新增（基础体验大更新）
- 本地文件夹体验大更新，按钮功能补全，音乐文件夹内新增多选、排序、目录打开、加入播放列表，增加多种提示，完善体验
- 新增沉浸中心预览
- 歌词界面背景着色器更新，改为自制+三方双着色器，替代AMLL着色器，并且在歌词界面进行多个优化
- 音乐播放控制改进，增加或完善多种功能，如音高补偿、真随机播放、间隔循环、睡眠模式等，并且对应增加设置项
- 在线歌曲功能改进，网易云修复无法登陆问题，酷狗修复登陆账号为摆设，真正登陆的账号可以听高品质歌曲，以及vip歌曲，并优化许多在线歌曲的问题
- 跨平台改进，Linux增加Wayland与fcitx插件，解决Linux问题，MacOS适配窗口标题栏样式，在macos上显示左置的红绿灯
- 大幅优化性能，大幅采用多线程，移至c++进行计算，大幅简化QML框架，渲染性能更高，优化AOT编译占比。
- 补全一些设置项内容，补全一些内容
- 新增许多快捷键

### 其他
- 修复一些Bugs，修复一些平台上的问题
- 规范项目许可证
