# Music Player

基于 Qt 6（Widgets + Multimedia）与 TagLib 的 C++ 本地音乐播放器，面向 macOS。

## 功能特性

### 播放列表

- 右键菜单：添加文件 / 添加文件夹（递归，自然排序，数字段按数值比较，同目录天然分组连续）；
  支持从 Finder 拖拽文件/文件夹进窗口添加
- 8 列信息（TagLib 读取）：文件名、专辑、序号、歌曲、大小、格式、音质（码率/采样率）、时长；
  表头右键勾选列显隐，点击表头按列排序（同列再点反向）
- 文件去重；Shift/Ctrl 多选；右键菜单：全选 / 移除 / 清理失效文件 / 撤销移除 / 睡眠定时 / 清空列表（需先全选才出现）；
  撤销也支持 ⌘Z 快捷键
- ⌘F 搜索：按文件名/歌曲/专辑实时过滤当前列表，清空自动收起

### 多播放列表

- 左侧栏管理多个命名播放列表，不同风格的歌曲分列表存放
- 侧栏右键菜单：添加文件 / 添加文件夹 / 新建列表 / 重命名 / 删除（至少保留一个列表，非空删除弹确认）；
  添加会先切到右键指向的列表；新建/重命名直接在侧栏行内编辑（支持中文输入法），激活列表蓝色背景+白字显示
- 添加/移除/清空只作用于当前激活列表；同一首歌可加入多个列表
- 切换列表不打断播放：正在播的歌在新列表中则正常标记，否则无标记但标题/封面/歌词保持
- 每个列表记忆离开时的位置，切回时恢复选中与滚动；侧栏每个列表右侧显示歌曲数，
  列表标题行显示列表名与「当前/总数」（如 107/303）
- 持久化：`~/Library/Application Support/MusicPlayer/MusicPlayer/playlists.json`，
  每次变更即时原子写入；首次运行自动把旧版 QSettings 播放列表迁移为「默认列表」
- 双击播放；列宽按基础比例随窗口缩放，拖动表头边界后以新布局为基准
- 无专辑标签时回退显示所在文件夹名

### 播放控制

- 6 种播放模式：顺序播放 / 列表循环 / 单曲循环 / 随机播放 / 专辑循环 / 专辑随机（专辑 = 同一父文件夹）
- 随机模式使用 Fisher-Yates 洗牌队列：一轮内每首歌只播一次；◀◀ 按回退历史返回
- ▶▶ / ◀◀ 遵守播放模式（循环环绕、专辑内环绕）；播放超过 3 秒按 ◀◀ 先回到本曲开头
- 播放速度 0.5x–2x（持久化）；睡眠定时 15–90 分钟，到时渐弱音量后暂停
- 进度条拖动实时试听；播放出错提示并自动跳过坏文件（连续失败防死循环）

### 歌词

- 自动加载同名 `.lrc`：UTF-8 优先，失败按 GB18030 解码（系统 iconv）
- 支持一行多时间标签、`[offset:±ms]` 整体偏移
- 三行显示：上一句（暗色）/ 当前句（加粗大字）/ 下一句，二分定位当前行

### 专辑封面

- 内嵌图片优先：MP3（ID3v2 APIC）/ FLAC（Picture）/ M4A（covr 原子）
- 同目录回退：`同名图片`、`cover`、`folder`、`front`、`album` × jpg/jpeg/png/webp
- Qt 官方包缺 webp 插件时自动走 macOS ImageIO 解码（见「实现备注」）
- 左侧信息栏：封面 84×84 + 歌手名（白色偏灰）/ 歌曲名（加粗），占宽度 20%，歌词占 80%

### 交互细节

- 滚动条上蓝色短线标记当前播放位置；点击凹槽直接跳转
- 当前播放行：文件名/歌曲两列深蓝加粗显示
- 浏览列表（选中/滚动）空闲 30 秒后自动回到当前播放曲
- 启动时恢复上次播放位置（按路径定位）；播放列表里已删除的文件自动过滤

### 键盘快捷键

| 按键 | 功能 |
| --- | --- |
| `↑` / `↓` 或 `W` / `S` | 选中上/下移动 |
| `空格` | 播放 / 暂停 |
| `回车` | 播放选中曲目 |
| `,` `<` / `.` `>` | 上一首 / 下一首 |
| `⌘F` | 搜索 / 过滤 |
| `⌘Z` | 撤销移除 / 清空 |
| `←` / `→` | 音量 -5% / +5% |
| `Fn+↑` / `Fn+↓`、媒体音量键 | 音量增减 |

> 按键通过 nativeVirtualKey 识别，中文输入法下同样生效。

### 状态记忆

- 播放列表（含激活列表）→ `playlists.json`（JSON，即时保存）
- 表格元数据缓存 → `meta_cache.json`（mtime 失效重读，启动/切列表免全量解析标签）
- 窗口几何、音量、播放模式、当前曲目与播放位置 → QSettings

## 环境要求

- macOS 12.3 Monterey (Intel x86_64)
- CMake ≥ 3.21（本机 4.4.3）
- Apple clang（本机 13.1.6，C++17）
- Qt 6.5.3（官方预编译包，位于 `~/Qt/6.5.3/macos`）
- TagLib 2.3.1（brew 源码编译）

## 构建与运行

```sh
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.5.3/macos" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/music_player
```

> 目录搬移后 CMake 缓存会指向旧路径，删除 `build/` 重新 configure 即可。

## 工程结构

| 文件 | 说明 |
| --- | --- |
| `main.cpp` | 全部界面与逻辑（列表、播放模式、歌词、封面、快捷键、状态记忆） |
| `cover_mac.mm` | macOS ImageIO 封面兜底解码（Qt 插件缺失的格式，如 webp） |

## 实现备注（踩坑记录）

- **封面 webp 解码**：Qt 官方 6.5.3 mac 包 imageformats 插件仅含 gif/ico/jpeg/svg，
  无 libqwebp，FLAC 内嵌 webp 封面会导致 `QImage::fromData` 失败。
  已内置 ImageIO（macOS 11+ 原生支持 webp）兜底解码。
- **TagLib 2.3.1 API**：`MP4::Tag::cover()` 已移除，封面改从
  `tag()->itemMap()["covr"].toCoverArtList()` 获取。
- **Qt 6.5.3 限制**：无 GBK 文本编解码（歌词用系统 iconv 转换）；
  `QBoxLayout::addLayout` 无 alignment 重载（6.7 才有），用 `setAlignment()` 代替。
- **macOS 键映射**：中文输入法下 Cocoa/Carbon 键映射不一致，QShortcut 可能失效，
  键盘操作统一走事件过滤器（nativeVirtualKey），仅媒体音量键保留 QShortcut。

## 环境搭建备注

- Homebrew 6 已停止为 macOS 12 (Monterey) 提供预编译 bottle，`brew install qt`
  会触发 Qt 源码全量编译（不可行）。因此 Qt 改用 `aqtinstall` 下载官方预编译包：
  `aqt install-qt mac desktop 6.5.3 clang_64 -m qtmultimedia -O ~/Qt`
- 清华 Qt 镜像（tuna）上 `qtdeclarative` 归档校验和不符（2026-08-30），
  改用官方源（会自动重定向到 mirrors.sau.edu.cn）。
- pip 受系统代理（127.0.0.1:7890，Clash）影响，代理未运行时需
  `env no_proxy='*'` 绕过。
