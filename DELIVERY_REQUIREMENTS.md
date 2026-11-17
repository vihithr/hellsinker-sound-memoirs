# Hellsinker Sound Memoirs 交付需求文档

- 文档状态：草案
- 更新时间：2026-11-17
- 当前版本：v1.1.0
- 文档范围：现有 Windows 交付、GitHub 发布流程，以及尚未确认的跨平台与 Web/WASM 交付范围

## 1. 文档规则

- 本文只记录用户已确认的要求、当前代码/仓库事实，以及必须在实施前确认的事项。
- 本文不包含方案推荐、优先级判断、工作量判断或未经确认的技术选型。
- 标记为 `TBD` 的内容不视为已批准需求。
- 未经用户确认，不得将 `TBD` 项作为默认需求进入开发或发布。

## 2. 当前交付基线

以下内容为当前仓库和已发布版本的事实记录：

- 项目名称：`Hellsinker Style BGM Player`。
- 当前发布版本：`v1.1.0`。
- GitHub 仓库：`https://github.com/vihithr/hellsinker-sound-memoirs`。
- 默认分支：`main`。
- Windows 发布产物：
  - `hsbgm.exe`
  - `hsbgm-win32.zip`
- 发布流程：
  - GitHub Actions 工作流文件：`.github/workflows/build.yml`。
  - `main` 分支 push 会执行构建。
  - `v*` tag push 会构建并创建 GitHub Release。
- 当前 Windows 构建环境：MSVC x86。
- 当前程序界面使用 Win32、GDI、`waveOut` 和 Media Foundation。
- 当前可执行文件为静态链接，运行时不依赖额外 VC 运行库。

## 3. 已确认需求

### DR-001 GitHub 分发

- 发布渠道为 GitHub Releases。
- 当前范围不包含 App Store、Google Play 或其他应用商店上架。
- 因商店上架产生的注册费、审核流程和商店分发材料不属于当前需求。

### DR-002 数字发布版本

- 发行版本使用 `v*` 数字 tag。
- 新版本必须通过 GitHub Actions 构建和发布。
- GitHub Release 必须包含可执行产物。

### DR-003 Release Notes

- Release 描述必须明确列出该版本新增、修改或修复的用户可见内容。
- 不得只提供打包说明、自动生成的 changelog 链接或无内容摘要。

### DR-004 LIB 曲名字体

- LIB 曲目列表中的曲目标题支持两种模式：
  - `ORIGINAL`：原版英文字格 + Fusion Pixel CJK 回退。
  - `PIXEL`：纯 Fusion Pixel。
- 默认模式为 `PIXEL`。
- 设置项名称使用 `ORIGINAL / PIXEL`。
- 切换范围仅包括：
  - LIB 曲目列表中的曲目标题。
  - LIB 底部艺人/专辑信息行。
- 曲目编号、搜索框、按钮和帮助文字不随该设置切换。
- 设置保存到 `hsbgm.ini` 的 `LibFont` 键。

### DR-005 窗口缩放

- 支持两种模式：
  - `FIT`
  - `INTEGER`
- 默认模式为 `INTEGER`。
- `F6` 用于切换 `FIT / INTEGER`。
- 右键菜单提供整数倍缩放勾选项。
- `INTEGER` 使用向下取整的整数倍率，并将画面居中。
- 当窗口小于 640×480 时，`INTEGER` 暂时回退到 `FIT`，不得裁切界面。
- 设置保存到 `hsbgm.ini` 的 `ScaleMode` 键。

### DR-006 频道栏空闲淡出

- 适用对象：`TR`、`LIB`、`SYS` 三个频道栏图标及其当前页/悬停指示。
- 无操作约 2.5 秒后开始淡出。
- 淡出持续时间约 1 秒。
- 完全淡出后不得留下明显色差矩形。
- 以下操作立即恢复频道栏：
  - 鼠标移动
  - 鼠标点击
  - 鼠标滚轮
  - 键盘输入
  - 窗口缩放
  - 文件拖入
- 鼠标停留在频道栏区域时保持显示。

### DR-007 SYS 面板

- SYS 面板包含四行设置：
  - 输出音量
  - 画面配色
  - 界面语言
  - 曲库字体
- SYS 第四行使用 `ORIGINAL / PIXEL`。
- 默认值文本应与实际默认值一致。

### DR-008 多语言

- 当前界面语言包括：
  - 英语
  - 简体中文
  - 日语
- 语言设置保存到 `hsbgm.ini` 的 `Language` 键。

### DR-009 本地曲库

- LIB 支持本地音频文件。
- 支持通过文件选择、文件夹导入和文件拖入添加曲目。
- 支持按文件名、曲名、艺人、专辑搜索。
- 支持播放、暂停、停止、跳转、上一首、下一首。
- 支持 `ORDER`、`ONE`、`LOOP`、`SHUFFLE` 四种顺序模式。
- 支持移除曲库条目、调整顺序和按文件名排序。
- 曲库列表、当前选择和顺序模式持久化到 `hsbgm.catalog`。

### DR-010 游戏曲库

- TR 读取用户本机 HellSinker 安装目录中的 PAK 曲目。
- 程序不随仓库分发游戏音频。
- 当前支持的游戏数据目录通过 `GameDir` 或 `--gamedir` 指定。

### DR-011 日文标签

- 支持旧式 CP932 编码 ID3v1 日文标签恢复。
- 修复不得改写用户原始音乐文件。

## 4. 当前代码约束

以下为当前代码事实，不代表已批准的架构改造需求：

- `src/ui.cpp` 同时包含窗口过程、页面状态、曲库视图、SYS 视图、输入处理、渲染和部分业务行为。
- `src/config_panel.inl` 被直接包含在 `src/ui.cpp` 内，并依赖 `ui.cpp` 的局部作用域。
- `src/catalog.h` 直接包含 `<windows.h>`。
- `src/catalog.cpp` 同时处理曲库持久化、标签读取、Windows Shell 元数据、目录选择对话框和旧 ID3 编码恢复。
- `src/audio.h` 直接包含 `<mmsystem.h>`，公开类型包含 `HWAVEOUT`、`WAVEHDR` 和 `CRITICAL_SECTION`。
- `src/panel_text.h` 直接依赖 Win32 GDI 的 `HDC`、`RECT` 和 `COLORREF`。
- `src/catalog.cpp` 中包含 `static_assert(sizeof(wchar_t) == 2, ...)`。
- 当前曲库文件使用 UTF-16 路径数据，并以 Windows `wchar_t` 为 2 字节作为前提。

## 5. 跨平台与 Web/WASM 待确认项

以下事项尚未由用户确认，不得默认进入交付范围：

| ID | 待确认事项 | 状态 |
|---|---|---|
| TBD-001 | 下一交付目标平台：Linux、Android、iOS、Web/PWA 或其他 | TBD |
| TBD-002 | 平台交付顺序 | TBD |
| TBD-003 | 是否保留 Windows、Web、Linux、Android 多端并行维护 | TBD |
| TBD-004 | Web 端是否使用自定义域名或 `github.io` 地址 | TBD |
| TBD-005 | Web/WASM 的最低浏览器及版本范围 | TBD |
| TBD-006 | Web/WASM 是否支持目录授权持久化 | TBD |
| TBD-007 | Web/WASM 是否支持复制音乐到浏览器私有存储 | TBD |
| TBD-008 | Web/WASM 是否需要离线 PWA | TBD |
| TBD-009 | Web/WASM 是否需要移动浏览器支持 | TBD |
| TBD-010 | Web/WASM 是否需要后台播放和系统媒体控制 | TBD |
| TBD-011 | Web/WASM 是否支持直接从本机导入 HellSinker PAK | TBD |
| TBD-012 | WASM 仅复用核心还是承载整套应用 | TBD |
| TBD-013 | 是否引入 SDL、Emscripten 或其他跨平台运行时 | TBD |
| TBD-014 | 是否支持多线程 WASM 及其部署响应头 | TBD |
| TBD-015 | iOS 是否要求免商店安装、侧载或 TestFlight | TBD |
| TBD-016 | 各平台支持的音频格式清单 | TBD |
| TBD-017 | 各平台的文件选择、目录扫描和持久化策略 | TBD |
| TBD-018 | 跨平台重构是否作为独立版本交付 | TBD |
| TBD-019 | 跨平台重构期间的 Windows 兼容性要求 | TBD |
| TBD-020 | 各平台的最低系统版本和硬件要求 | TBD |
| TBD-021 | 新增平台的交付物、安装包格式与文件命名 | TBD |

## 6. 交付物要求

- Windows 当前交付物见第 2 节。
- 新增平台的交付物、包格式、构建目标和文件命名由 TBD-021 确认后补充。

## 7. 验收要求

### 7.1 Windows 现有功能

- 构建成功。
- `--selftest` 通过。
- 可播放游戏曲库与本地曲库。
- 曲库持久化可恢复。
- `ORIGINAL / PIXEL` 切换作用于规定范围。
- `FIT / INTEGER` 切换与持久化正常。
- 频道栏淡出、恢复和无色差消失正常。
- 中日英语言切换正常。

### 7.2 GitHub 发布

- Tag 推送后 Actions 成功完成构建。
- Release 包含约定的全部产物。
- Release Notes 包含本版本用户可见变更。

### 7.3 新增平台

- 在 TBD-001 至 TBD-020 中的相关项目确认后补充可执行验收标准。
- 未确认对应平台范围前，不创建该平台验收项。

## 8. 不包含事项

- App Store、Google Play 或其他商店上架。
- 商店注册费用、审核材料和商店专用发布包。
- 分发游戏音频或绕过用户本机授权数据。
- 未经确认的平台移植、架构改造或技术选型。

