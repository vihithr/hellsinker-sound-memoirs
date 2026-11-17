# WASM 播放器视觉 1:1 复刻研究

2026-11-17

分支：`research/wasm-visual-parity`

基线：`56cd7134bd6a77b1ceb231bf60b2b47319b01ae8`

## 结论

当前问题不是“Canvas 重画得还不够像”，而是 Windows、`wasm/`、`web/` 三套入口没有共同定义“某一逻辑时间点应该产生哪些像素”。

- `wasm/` 已经有正确的 PAK、解码和 43 段频谱核心，但 `wasm/screen.js` 只是按原版 640×480 布局手写的 Canvas 子集。它复用数据，不复用绘制语义。
- `web/` 是独立 DOM/CSS 视觉实验，时钟、频谱、字体、布局、状态和交互都不同于 Windows，不应被当作正式视觉主线。
- 真正可能做到逐像素 1:1 的路线只有一条：抽出平台无关的软件光栅器，由 Windows 和 WASM 共享同一份 RGBA 输出。Windows 端最后只负责把该缓冲区呈现到窗口，Canvas 端最后只负责把同一缓冲区复制到画布。
- 如果所谓“共用”只是 C++ 导出高层 draw list，Windows 继续用 GDI、浏览器继续用 Canvas 2D 解释，仍会分别在字体、线条、多边形、alpha 和缩放上漂移。这只能保证命令和布局一致，不能保证像素一致。
- 纯 JS/CSS 重做适合做响应式产品、主题和交互实验，不能作为 1:1 验收主线。

保守的第一步不是立刻重写全部界面，而是建立固定状态、固定时钟、固定像素尺寸的 reference frame 采集和 golden test。没有这套基线和容差，后续任何“更像”都无法证明。

## 仓库事实

仓库文档已经把三个入口的角色写清：

- `docs/architecture.md:3` 明确“桌面播放器是行为基准”；`wasm/` 是共享 C++ 核心的 TR 播放器；`web/` 是独立视觉实验。
- `docs/visual-motion-roadmap.md:24` 要求绘制只读状态，暂停冻结，停止清空，跳转或换曲丢弃旧残影。这是设计意图，不是当前三端共同实现。
- `wasm/README.md` 明确当前只复用 PAK、解码、频谱和 640×480 布局，不包含 SYS、持久化、完整启动动画等。
- `web/README.md` 明确 `web/` 是零构建本地音乐播放器原型，不是 WASM 主线。

核心边界目前基本成立：

- `src/core/spectrum.*` 只负责 FFT、波形、电平和平滑状态。
- `wasm/core/api.cpp` 和 `wasm/worker.js` 没有 Canvas 或 DOM 依赖。
- `wasm/screen.js` 在浏览器主线程读取频谱快照并绘制。
- `web/app.js` 不调用共享 C++ 频谱核心，而是使用 Web Audio `AnalyserNode`。

### Windows 画面入口

Windows 的实际帧入口是 `src/ui.cpp:PaintScene`，绘制顺序为：

1. 背景。
2. 左右侧翼。
3. 面板边框，带启动展开和全屏暗场淡出。
4. ORIGINAL 模式频谱，或其他 F7 stage scene。
5. 标题。
6. TR、LIB、SYS 页面内容。
7. 页面 tab、hover 提示、切页扫线。

`PaintScene` 不是纯视觉函数，它直接读取全局 `g_app`、`Page`、`tick`、主题、局部曲库选择和 native 音频状态。`src/ui.cpp:1076` 到 `src/ui.cpp:1129` 是当前耦合最重的视觉代码段。

### WASM 画面入口

`wasm/app.js` 负责文件、播放、状态和 60 Hz 调度；`wasm/screen.js` 是固定 640×480 Canvas 子集。它没有共享 Windows 的 `PaintScene`、主题、stage、library、system、文本换行或裁剪逻辑。

`wasm/screen.js` 实际绘制：

- 背景、左右电平条和 240 点波形。
- 主面板边框、标题位图。
- 43 段频谱、band seam 和 axis 位图。
- 两行播放信息。
- “STOP”和 46 个 TR 格子。

当前没有绘制：

- 启动时边框展开、频谱淡入、全屏暗场淡出。
- 选择框 5 帧脉冲和白色扫描条。
- 24 帧提示闪烁。
- F7 stage 和 F8 wing。
- LIB、SYS、hover、tab 隐藏、切页扫线。
- Windows 的主题重映射。
- Windows 的动态 CJK 文本和完整状态文案。

### Web 视觉入口

`web/index.html` 和 `web/styles.css` 是响应式 DOM/CSS 版式，不存在 640×480 逻辑画布。`web/app.js:1016` 到 `web/app.js:1182` 单独实现了一个 Canvas 2D 频谱区，但它只是页面中的一个响应式模块。

`web/` 的视觉与播放算法都独立：

- `<audio>` 提供媒体和播放时钟。
- `AudioContext` 中的 `AnalyserNode` 提供 `fftSize=2048`、`smoothingTimeConstant=0.82` 的浏览音频数据。
- Canvas 自行做 43 段指数频带映射，不是 `src/core/spectrum.cpp` 的固定 bin 区间和整数压缩。
- 左右“能量”实际由频带低半区和高半区相加得到，不是左右声道包络。
- 暂停时进入基于墙钟的正弦 idle 动画，不是冻结最后一帧。
- rAF 每帧直接衰减 peak，刷新率越高，衰减越快。

因此 `web/` 只能是设计参考和交互试验，不能并入视觉 golden 主线。

## 当前真实视觉契约

### 1. 表面、逻辑坐标和缩放

Windows 以 640×480 作为几何基准，但没有一个覆盖所有绘制路径的统一变换：

- `src/ui.cpp:29` 到 `src/ui.cpp:57` 定义 640×480、面板、频谱、标题、格子和信息区坐标。
- `src/ui.cpp:852` 到 `src/ui.cpp:881` 计算 `g_scale = min(clientW/640, clientH/480)`，默认整数模式只在 `g_scale >= 1` 时向下取整。
- 回缓冲区尺寸是 `round(640*g_scale) × round(480*g_scale)`。
- 大多数 `FillRect`、`StretchBlt` 和文本矩形通过 `S()` 手动取整。
- `src/stage.cpp:94` 到 `src/stage.cpp:99` 对 stage 临时设置 `MM_ANISOTROPIC`，逻辑窗口 640×480，viewport 为缩放后的回缓冲区。
- `src/panel_text.cpp` 切回 `MM_TEXT`，按目标像素矩形和 `g_scale` 重新栅格化文本。

这说明 Windows 的所谓“逻辑坐标”是混合语义，不是简单的 640×480 矢量画布：

- 大多数几何坐标乘以 `g_scale`。
- 部分 GDI pen 宽度仍是固定的物理像素。例如侧翼波形创建 2 像素 pen，外框 HLine/VLine 也用固定宽度。
- `stage.cpp` 使用 stock cosmetic pen，坐标被 map mode 放大，但线条厚度和 join 行为不是 Canvas `lineWidth` 的直觉等价物。
- CJK 字体不是把 12 px 位图整体放大，而是按 `Scaled(12, g_scale)` 重新创建 GDI 字体。

仓库没有设置进程 DPI awareness，也没有处理 `WM_DPICHANGED`，没有 manifest DPI 声明。仓库事实只能证明“客户端坐标和自算 scale”，不能证明 Windows 在 125%、150%、200% 系统缩放下得到稳定物理像素。参考采集必须在明确的 DPI awareness 和系统缩放条件下进行，不能直接截桌面窗口后假定可复现。

WASM 当前不是“高 DPI 画布”：

- `wasm/index.html` 固定 `canvas width=640 height=480`。
- `wasm/screen.js` 只在 640×480 上绘制。
- CSS 用 `width:min(100%, calc(80vh * 1.3333))` 和 `height:auto` 展示，再用 `image-rendering:pixelated` 交给浏览器合成。
- `devicePixelRatio` 不参与 Canvas backing store 或绘制精度。

实际浏览器探针结果：

| viewport | DPR | Canvas buffer | Canvas CSS 尺寸 | CSS/buffer 比例 |
| --- | ---: | --- | --- | --- |
| 390×844 | 1 | 640×480 | 378×283.5 | 0.590625 |
| 390×844 | 2 | 640×480 | 378×283.5 | 0.590625 |
| 640×480 | 1 | 640×480 | 511.984×383.984 | 约 0.8 |
| 850×600 | 1 | 640×480 | 639.969×479.969 | 约 1.0 但非精确 |
| 1280×960 | 1 | 640×480 | 818×613.5 | 1.278125 |
| 1280×960 | 2 | 640×480 | 818×613.5 | 1.278125 |

因此当前 Canvas 的内部像素在所有 DPR 下相同，最终显示由 CSS 和合成器重采样。`image-rendering:pixelated` 只是展示提示，不能保证浏览器、设备、DPR 下的采样位置和 GDI `StretchBlt(COLORONCOLOR)` 一致。

### 2. 位图、字体和颜色

Windows 的文本和图像有两条不同路径：

- ASCII 主要走 8×11 单元格图集。`src/ui.cpp:327` 到 `src/ui.cpp:341` 用 `StretchBlt(COLORONCOLOR)`。
- 主题图集来自同一 BMP 源，`tools/make_themed_art.py` 生成五个主题的 `cells_*.bmp`，并用最近邻遮罩重新上色。
- `src/panel_text.cpp` 对图集做二值化 mask 和颜色 strip，通过 `AlphaBlend` 绘制。非 ASCII 文本使用内嵌 Fusion Pixel 字体，`NONANTIALIASED_QUALITY`。
- 标题不是直接每次缩放原始位图。`tools/make_art_runs.py` 把 `assets/memoirs-title.bmp` 的两个 pigment 色追踪成 run-length 矩形，`src/ui.cpp:719` 到 `src/ui.cpp:722` 在逻辑坐标中画矩形。
- 当前 `PaintSpectrum` 没有使用 `memoirs-axis.bmp`；频率轴线由 rect 画，标签由单元格图集画。

WASM 的资产链不同：

- `tools/make_wasm_assets.py` 直接把 `themed/cells_0.bmp`、`memoirs-title.bmp`、`memoirs-axis.bmp` 转成 PNG。
- `wasm/screen.js` 将 418×56 title 直接缩小到 209×28，将 688×24 axis 直接缩小到 344×12。
- `ctx.imageSmoothingEnabled=false` 只关闭图像插值，不会把 title 变成 Windows 的两色 run；也不会把 axis 变成 Windows 的 vector line 加 atlas labels。

实测资产：

| 资产 | 尺寸/模式 | 颜色数 |
| --- | --- | ---: |
| `assets/memoirs-cells.bmp` | 376×33 RGB | 13 |
| `assets/memoirs-title.bmp` | 418×56 RGB | 223 |
| `assets/memoirs-axis.bmp` | 688×24 RGB | 944 |
| `wasm/assets/title.png` | 418×56 RGB | 223 |
| `wasm/assets/axis.png` | 688×24 RGB | 944 |

所以 title 和 axis 的颜色、边缘和采样已经不是 GDI 输出的同一种栅格化。静态截图对比只是把这个事实显示出来，不是偶发的 CSS 模糊。

颜色契约也有明显差异：

- Windows 主色来自 `src/themes.generated.h` 的 `COLORREF` 和 `assets/palettes.json`，有五个主题。
- GDI `FillRect` 和 bitmap blit 在大多数非 alpha 区域输出离散源色。
- GDI `AlphaBlend` 对淡入、选择扫描条和 tab 渐隐做常量 alpha 混合。
- Canvas 路径默认 antialias。`strokeRect`、`stroke`、`lineTo` 会在边缘产生混合色，即使 `fillStyle` 是同一个 hex。
- `web/` 还使用 `color-mix()`、透明度、渐变、阴影和 CSS transition，主动生成 Windows 不会有的中间色。

### 3. 裁剪

Windows 当前至少有三种裁剪：

- stage scene 在 `src/stage.cpp` 中对 148,80 到 492,238 做 `IntersectClipRect`。
- `DrawPanelText` 对每个文本目标矩形做 `IntersectClipRect`，再做省略号布局。
- 主窗口通过回缓冲区尺寸和最终 `BitBlt` 裁掉越界内容。

`wasm/screen.js` 只有 Canvas 边界和 draw image 的隐式边界，没有 stage clip、文本矩形 clip、Windows 的省略号算法或 native child edit 控件语义。`web/` 的 `.scope` 有 CSS `overflow:hidden`，文本有 `text-overflow:ellipsis`，但它们是另一套排版规则。

### 4. 帧时序

Windows：

- `src/ui.cpp:1560` 到 `src/ui.cpp:1564` 用 `SetTimer(hwnd, 1, 16, 0)`，并 `timeBeginPeriod(1)`，意图接近 60 Hz。
- `OnTimer` 每个 timer 消息推进 `app.tick`，更新频谱，推进 stage history，然后 `InvalidateRect`。
- `tick` 是界面帧计数器。面板展开 64 tick、选择脉冲 5 tick、提示 48 tick、tab 渐隐 60 tick、切页扫线 12 tick 都直接依赖它。
- GDI `WM_TIMER` 可能合并、延迟，`WM_PAINT` 也可能合并。所谓 60 Hz 是行为目标，不是硬实时保证。
- `stage::Memory::advance` 在 `src/stage.cpp:14` 到 `src/stage.cpp:25` 还有 45 ms 最小间隔，实际历史采样通常约 22 Hz，而不是每个 16 ms timer 都新增一层。

WASM：

- `wasm/app.js:100` 用 rAF 驱动。
- `wasm/app.js:88` 到 `wasm/app.js:99` 试图以 16.166−0.5 ms 为门槛，每次满足才重算频谱和重绘。
- 在 120 Hz 显示器上通常隔帧绘制，在 30 Hz 设备上实际只更新 30 次。
- 浏览器后台标签页会暂停 rAF，音频仍继续；恢复时 `last` 可能跨过很长时间。
- Canvas 动画、频谱状态和播放时钟没有统一的 `tick` 模型。

Web：

- `requestAnimationFrame(draw)` 每帧更新 peak。
- idle 波形使用 rAF 时间戳生成正弦。
- CSS `breathe` 和 `meter transition` 是另一套浏览器动画时钟。
- `prefers-reduced-motion` 会改变动画时长。

因此“60 Hz 原版”、“rAF 刷新率”、“CSS 动画时间”和“音频 PTS”现在是四个独立时间源。

### 5. 频谱和音画状态

共享 C++ 频谱的实际契约在 `src/core/spectrum.cpp`：

- FFT 使用 1024 帧。
- `visualCursor` 从播放秒数换算到 16-bit stereo frame，并减去 22050 个字节，即 125 ms 的视觉补偿。所有 bars、wave、meter 共用这个 cursor。
- 43 个柱使用固定分组区间，不使用普通对数频率轴。
- 中间组 24..29 先和旧状态平均，再经过 96、64 两级压缩。
- `bars = (level + 11 * old) / 12`，`barsPrev = old`，`barsLevel = level`。
- `waveL` 从右声道样本得到，`waveR` 从左声道样本得到。左右表在命名和显示上是交换过的，电平条也沿用该交叉关系。
- `envL` 取右声道，`envR` 取左声道。
- `analyze` 是有状态平滑，不是纯函数。连续对同一 PCM 窗口调用会继续改变结果；输入帧数为 0 才把结构体清零。
- WASM ABI 的 611 个 float 顺序为 bars、barsPrev、barsLevel、waveL、waveR、envL、envR。

Windows：

- `UpdateVisualization` 在 `audio.isPlaying()` 时调用 `analyze`，否则用零帧清空。
- 暂停时 `AudioEngine::isPlaying()` 仍返回真，因为 `playing_` 没被清除；`isPaused()` 单独表示冻结。于是暂停期间位置不变，但 `analyze` 每个 timer 仍对同一 1024 帧重复平滑，bars 和 env 会继续收敛，不是真正的冻结。
- `stageMemory.advance` 显式接收 `isPlaying() && !isPaused()`，因此 stage ridge/trace 会冻结。
- 停止后 `current=-1`，下一次 timer 用零帧清空频谱。
- 拖动进度时只即时重画 seek rail，音频和频谱仍在旧位置继续；抬起后才 `audio.seek()`。频谱下一次分析从新位置取帧，但没有先清空 bars 平滑状态。stageMemory 只有在新旧位置差值超过阈值时才丢弃历史。

WASM：

- 暂停通过 `detach()` 停掉 source，但保留 buffer 和 `current`。
- 因为暂停后没有 `source`，`wasm/app.js` 走 `_sm_spectrum_reset()`，频谱立即清零。
- stageMemory 不存在，所以谈不上冻结和丢弃残影。
- 拖动 seek 时正在播放的 source 不会暂停；拖动过程中 Canvas 仍显示旧播放位置，松开后才设置 offset 并重启 source。
- 换曲选择只改变 `selected`，不自动停止当前 `current`；点击结果按 UI 不同也会直接激活。
- 游戏 TR 曲循环，本地曲不自动切下一首。

Web：

- 暂停时 `isLive=false`，立即切换为 idle sine 波形和低频带能量。
- `AnalyserNode` 是浏览器内部平滑，不是 C++ 的 125 ms cursor、固定 bin 和平滑公式。
- 拖动 seek 时用 `timeupdate` 和 pointer 事件更新 UI，和 Canvas 的 rAF 频谱不是同一个 PTS。

### 6. 状态机对照

| 事件 | Windows | WASM Canvas | Web 实验 |
| --- | --- | --- | --- |
| 空闲 | `current=-1`，下一秒 timer 清空频谱 | `visual=null`，每帧 reset | 持续 idle 正弦动画 |
| 播放 | 每 timer 用 audio position 更新频谱；stage 约 45 ms 推进 | 满足约 60 Hz 门槛时更新 | rAF 每帧更新 Analyser |
| 暂停 | 位置冻结；bars/env 仍平滑；stage 冻结 | source detach；频谱清零 | 切成 idle 动画 |
| 停止 | 清音频、current；下一 timer 清视觉 | detach 且清 buffer/current；频谱清零 | 清 audio source，UI 回 idle |
| 拖动 seek | 立即更新进度条；音频继续；release 后 seek | 播放继续；松开才 seek；Canvas 无进度轨 | 立即写 audio.currentTime，进度 UI 更新 |
| 切曲 | 单选择不等于播放；Enter/双击启动 | 选择不等于播放；网格单击可直接启动 | 点击列表立即 load/play |
| 曲尾 | 本地曲按 order 逻辑切下一首；TR 循环 | 本地曲停在末尾，TR 循环 | repeat/next 独立实现 |
| F7/F8 | 有 4 个 stage、5 个 wing | 无 | 无 |
| 主题 | 5 套 shared theme | 只有 ORIGINAL | 2 套 CSS theme |

上表不是为了说明谁“错”，而是说明当前没有同一状态契约。任何视觉 golden 必须先固定这张表。

## 漂移分成五层

### 第 1 层：内容覆盖

WASM 只画了 Windows TR 页的一部分。先把缺失内容列清，不能用“主界面差不多”代表播放器完成：

- 启动动画和 alpha。
- selection 脉冲、stripe、hint blink。
- stage 和 wing。
- LIB/SYS。
- hover、tab 渐隐、切页扫线。
- 不同主题和完整文本/状态。

这一层即使完美手写，也只是形似，因为下面的栅格化仍不同。

### 第 2 层：状态和时钟

Windows 的 `tick`、audio position、`isPaused`、stage memory 和 spectrum smoothing 是五个不同状态。WASM 当前把“暂停”实现成“没有 source”，导致视觉态被错误地并入播放态。Web 又把暂停并入 idle。必须先定义状态快照，再谈重画。

### 第 3 层：绘制语义

Windows 不是一份纯几何 display list：

- 位图 pattern 和 `StretchBlt`。
- GDI pen/线宽/join。
- `Ellipse`、`Polyline`、`Polygon` 的扫描转换。
- `AlphaBlend` 的 alpha 舍入。
- GDI 字体 hinting 和 `NONANTIALIASED_QUALITY`。
- `IntersectClipRect`。

Canvas 2D 对相同“圆心、半径、线宽”会产生不同的像素。CSS 的路径抗锯齿、字体 hinting、DPR 合成又是浏览器和 GPU 决定。直接按视觉参数重画，只能靠目测接近。

### 第 4 层：资产

当前 title、axis 已经证实采用不同资产链。即使所有坐标完全一致，title 的 223 种源色和 Windows 两色 run、axis 的 944 色贴图和 atlas 文本仍会产生稳定差异。

### 第 5 层：呈现和 DPR

Canvas buffer 固定 640×480，CSS 比例由 viewport 决定。即使 Canvas 内一像素完全正确，合成后仍可能：

- 被缩小丢掉整列或整行。
- 被非整数放大并产生采样不均。
- 在 DPR 2 以上由浏览器选择重复列或边缘混合。
- 与浏览器地址栏/边框偏移结合后落在半像素。

Windows 本身也受 window client 尺寸、DPI virtualization 和 GDI 模式影响。只有在明确 target surface 和 presentation scale 下，“1:1”才可定义。

## Reference frame 和 golden test

### 固定状态

建议新增一个仅用于测试的 `VisualState`，它不嵌入音频核心，也不直接持有 HWND/DOM：

```text
schema
surface: width, height, integer_scale, theme, colorspace
clock: ui_tick, audio_pts, playing, paused, seeking, track_id
screen: page, selected, current, loading, hover
spectrum: bars, barsPrev, barsLevel, waveL, waveR, envL, envR
stage: mode, wing_mode, memory
```

测试状态由文件读入，不能从真实时钟、真实鼠标或真实声卡临时推导。建议每个 fixture 包含：

- 原始 1024×2 i16 stereo PCM。
- 连续 `analyze` 次数。
- 期望的 611 float spectrum 快照。
- `VisualState` JSON/BIN。
- 期望的 640×480 RGBA8 原始输出。
- 期望的 1280×960 或 1920×1440 整数倍输出。

这样可以把“频谱核是否正确”和“renderer 是否正确”分开定位。

### 最小帧集

第一批不应覆盖所有音乐，而应覆盖所有状态边界：

1. `tick=0` 启动暗场。
2. `tick=31` 淡出中段。
3. `tick=64` 完全展开。
4. selection 的 5 个 tick。
5. hint 的 0/24/48 tick。
6. 静音、弱信号、强低频、左右声道明显分离。
7. 暂停后的第一个 tick 和暂停保持 60 tick。
8. 正向和反向 seek，包含小于 100 ms、100 ms 到 1 s、大于 1 s 三种。
9. 换曲，旧 track memory 必须丢弃。
10. stop 后第一帧，频谱和 stage 必须清空。
11. F7 四种 mode、F8 五种 wing 的代表帧。
12. ORIGINAL、DARK、NEON、PAPER、BLUEPRINT 的同一静态帧。

### 采集方式

Windows 参考帧不要截整个桌面窗口，避免 window border、DWM、缩放和颜色管理混入。应在进程内：

1. 固定 DPI awareness 和系统缩放，或直接走 offscreen DIB。
2. 建立 640×480 或整数倍 RGB/BGRA DIB。
3. 调用与正式路径相同的 scene renderer。
4. 输出未压缩 RGBA/BGRA 和 PNG。
5. 同时输出状态摘要、palette hash、atlas hash 和 renderer version。

浏览器参考帧分两种：

- `canvas_frame`：直接读取 `getImageData(0,0,640,480)` 或 `toDataURL()`，验证画面内容。
- `presented_frame`：在固定 viewport、DPR、CSS 尺寸和整数缩放下用 headless browser 截屏，验证最终合成。

两者都必须有 golden。只看 `toDataURL()` 会漏掉 DPR/CSS 漂移；只看最终截屏会把 renderer 错误和 compositor 错误混在一起。

建议在 visual test mode 中强制固定布局，而不是沿用普通响应式页面：

```css
html, body { margin: 0; background: #000; }
#stage { width: 640px; height: 480px; overflow: hidden; }
#screen { width: 640px; height: 480px; display: block; }
```

对 DPR 2/3 测试，使用 `deviceScaleFactor` 和整数物理倍率，并在测试里读取 `devicePixelContentBoxSize`，确认每个逻辑像素真的对应整数个设备像素。不要只依赖 `image-rendering: pixelated`。

### 视频和同时间点

静态 golden 是硬门禁，视频只用于人工复核和端到端音频同步。推荐：

- 使用同一无损 WAV fixture 和同一状态 timeline。
- 记录每帧 `audio_pts`、`ui_tick`、`audio_frame_index`、`seek_epoch`、`track_id`。
- 用 audio PTS 而不是 wall clock 触发采样点。
- 为每段视频加数字帧号、理论 PTS 和 tick 的屏幕角落叠层，叠加层只能放在诊断视频，不能污染 golden。
- 用 FFV1 或 PNG 序列做差分；H.264 仅用于分享，不能做像素门禁。
- 同时间点截图固定到 `audio_pts` 相同、允许最多 1 个 60 Hz state step 的采集偏移；状态帧本身不能用容差掩盖。

音频侧建议把逻辑时钟定义为：

```text
source_pts = offset + (AudioContext.currentTime - started_at)
loop 时按 duration 取模
```

并以 `getOutputTimestamp()`、输出 latency 或 AudioWorklet 的 render quantum 校正实际 audible PTS。`offset` 必须在 seek/pause/restart 时原子更新。Canvas 的 `requestAnimationFrame(timestamp)` 只用于选择何时绘制，不用于推导音频位置。

### 像素容差

建议把容差分成“同 renderer”和“遗留实现对照”，不要用一条模糊阈值同时管理：

| 对比 | 硬门禁 |
| --- | --- |
| 同一 software renderer 的 native 与 WASM RGBA | 所有 RGB/alpha 像素完全相等，最大通道差为 0 |
| 同一 target scale 的 Windows DIB 与 browser raw frame | 完全相等 |
| GDI 旧路径到 software renderer 的迁移对照 | 结构差异不得出现；文字、图标和 1px 线先按区域逐项签字，最终仍要求 0 |
| 普通浏览器 presented frame | 必须验证整数倍率和无混合色；非整数响应式布局不进入 1:1 门禁 |
| `web/` 设计实验 | 可只用 SSIM、MAE 或人工评审，不能称为 parity |

过渡阶段如果需要可量化阈值，可临时使用：

- `pixelmatch` 阈值 0.05。
- p99 每通道差不超过 2。
- 变化像素不超过全帧 0.05%。
- 差异连通块不超过 1 px，且不得落在文字、图标、1px 线、selection 和 frame 区域。

这些只是迁移期告警，不是最终 1:1 定义。最终应以同一 canonical renderer 的 0 diff 为准。

## 架构候选

### A. 共享 portable software renderer

建议目标不是“共享命令、各自解释”，而是：

```text
VisualState
    |
    v
portable C++ software renderer
    |
    +-- RGBA8 640x480 / integer scale
            |
            +-- Windows: DIB + final blit
            +-- WASM: heap pointer + ImageData/putImageData
```

Windows GDI 只保留窗口、DPI、输入、最终呈现和 system UI。WASM 的 JS 只保留 DOM 控件、文件、音频和最终上传 Canvas。二者都不拥有 scene 的栅格化规则。

可以共享和必须固定的内容：

- 640×480 logical surface 和显式 scale spec。
- 半开区间 `fill_rect`、clip stack。
- 固定 nearest bitmap blit。
- 固定 line cap/join 和扫描转换。
- 固定 polygon/ellipse raster。
- 固定 alpha blend 舍入。
- 不依赖系统字体的 glyph atlas 或统一 rasterizer。
- 主题 palette。

“只共享 draw command、Windows 仍 GDI、WASM 仍 Canvas”不能归入真正 A。此时 renderer 不是共享的，只是 layout 共享。

优点：

- 唯一能同时固定布局、颜色、字体、裁剪、抗锯齿和动画像素的路线。
- 可以离线生成帧，测试不需要声卡或真实播放。
- Windows 和 WASM 可以用同一套 fixture 和 hash。
- Canvas 只做 `putImageData` 或 `ImageBitmap`，不会遇到 Canvas 路径抗锯齿差异。
- 后续 stage、theme、library 都可以增量迁移，不必一次性改 audio core。

代价：

- 需要写一个稳定的小型 2D rasterizer，不能直接调用 GDI 或浏览器 Canvas 路径。
- Windows 当前 GDI 输出会有一部分被重写成 canonical 像素，可能出现肉眼可见的细微变化。
- 需要决定 CJK 字体的最终策略。
- 需要处理 Windows DIB BGRA 与 Web RGBA 的顺序和 alpha。
- 每帧复制 1.2 MB RGBA 在 60 Hz 约 72 MB/s，现代浏览器可接受，但应使用持久 buffer，避免每帧分配。

### B. WASM 导出 display list，JS/Canvas 解释

把 scene 变成类似：

```text
fill_rect(x, y, w, h, color)
blit(atlas, sx, sy, sw, sh, dx, dy)
polyline(points, width, color)
text(atlas, glyphs, x, y, color)
```

由 JS Canvas 逐条执行。

适用场景：

- 调试原版绘制顺序。
- 做 DOM/Canvas 可视化检查器。
- 快速验证状态和布局。

它不能单独成为 1:1 方案，因为：

- Canvas 2D 的 `stroke`、`fill`、`arc`、`ellipse` 和 path antialias 不是 GDI 的扫描转换器。
- 字体和 CJK rasterization 仍是浏览器。
- 不同浏览器和 GPU 的合成结果可能不同。
- 如果 draw list 里带 `alpha`、渐变和纹理缩放，浏览器实现细节更多。

B 只有在把 display list 缩成“最终 RGBA 或预乘好的整数 tile blit”，并由 JS 做纯 `drawImage`/`putImageData` 时才可能达到 A 的结果。那时它实质上是 A 的传输层，不是独立的高层 Canvas 重绘架构。

### C. 纯 JS/CSS 按设计稿重做

优点是开发快、响应式、主题和可访问性好，适合 `web/` 这种实验入口。

但当前 Windows 画面依赖：

- GDI 的像素级 line/pen 行为。
- 8×11 atlas 的像素放大。
- GDI bitmap 和 `StretchBlt` 的颜色。
- Windows 字体 hinting。
- 固定 640×480 的布局和手写裁剪。
- 根据 tick 离散变化的选择、闪烁和 stage history。

CSS 的字体、subpixel layout、路径抗锯齿、渐变和 transition 会系统性地改变这些结果。即使逐项调参，也只能做到近似。C 可以是一个新的响应式播放器，不能承诺原版 1:1。

## 推荐架构

### 模块划分

建议最终拆成：

```text
src/visual/visual_state.h
src/visual/renderer.h
src/visual/renderer.cpp
src/visual/scene.cpp
src/visual/assets.cpp
src/visual/theme.cpp

src/core/spectrum.*              # 保持不变，输出视觉快照
src/ui.cpp                       # Windows window/state/adapter
wasm/core/api.cpp                # 保持音频/UI 无关
wasm/visual_api.cpp              # 新增 renderer C ABI，不碰音频 ABI
wasm/screen.js                   # 改为上传 RGBA，不再手绘 scene
web/                             # 继续独立实验，不并入 parity 门槛
```

ADT 草案：

```cpp
struct Surface {
    uint8_t* rgba;
    int width;
    int height;
    int stride;
};

void memoirs_visual_render(
    Surface& target,
    const VisualState& state,
    const RenderAssets& assets);
```

`VisualState` 是只读输入，绘制函数不修改 `spectrum`、`AudioEngine`、`tick` 或全局 page。动画状态推进放在一个明确的 `advance(state, frameStep)`，renderer 只消费快照。

### 时钟契约

建议把时钟拆成：

- `ui_tick`：固定 60 Hz 的整数状态步。暂停时仍可按原版推进界面脉冲。
- `audio_pts`：由音频输出时钟得到，驱动 `visualCursor`。
- `seek_epoch`：每次 seek/换曲递增，旧 stage memory 和旧 spectrum smoothing 必须丢弃。
- `paint_serial`：只用于浏览器和 Windows 的呈现去重，不参与视觉状态。

调度策略：

```text
accumulator += elapsed
while accumulator >= 1/60:
    advance_visual_state(1/60)
    accumulator -= 1/60
render_latest_state_on_paint()
```

不要在每个 rAF 都推进频谱或 peak。Canvas 可以每个 rAF 读取同一 state 并重绘，但状态不应因 144 Hz 显示器而加速。

暂停语义必须在 `VisualState` 中明确：

- `playing=false, paused=false`：清空或 idle。
- `playing=true, paused=false`：推进频谱和 audio-derived stage。
- `playing=true, paused=true`：保留最后一帧 audio-derived spectrum；是否继续 UI tick 单独由 `ui_tick` 决定。

如果必须完全复刻当前 Windows 的“暂停时 bars 仍收敛”，应该把这个 bug 级行为写进 legacy fixture，而不是靠 renderer 偶然产生。更合理的产品契约是 roadmap 中的“暂停冻结”，但需要明确版本和一次视觉基线变更。

### 字体策略

ASCII 首选继续使用现有 8×11 atlas，因为这是当前仓库最稳定、最可像素复刻的部分。

动态 CJK 有三条选择：

1. 仍使用 Windows GDI 和浏览器字体：无法保证 1:1，只能用于非 parity 的搜索输入/辅助界面。
2. 离线用 Windows GDI 生成所有需要的 12 px/24 px 单色 glyph atlas，再让两端共享：最容易贴近当前 Windows，但资产较大，动态字符覆盖和 fallback 需要约束。
3. 两端都使用固定版本、固定 hinting 的软件字体 rasterizer：跨平台一致，但会改变 Windows 当前 baseline，需要重新录制 golden，且必须固定库版本和编译选项。

不建议把浏览器字体交给 Canvas 后再期望与 GDI hinting 一致。

### Windows 呈现

Windows 第一阶段可以保留现有窗口和输入，但：

- 先加 offscreen frame capture，不改变正式画面。
- 再让 PowerShell/CI 在 640×480 和 1280×960 上比对 GDI 参考与 software renderer。
- 通过后把 `PaintScene` 切换为 `renderer.render()`，最后只用 `StretchDIBits`/`BitBlt` 显示 RGBA/BGRA。
- DPI awareness、`WM_DPICHANGED`、整数倍率和 letterbox 应成为表现层契约，不应藏在各个绘制函数里。

### WASM 呈现

建议新增独立的 visual ABI：

```text
visual_alloc(width, height) -> ptr
visual_render(ptr, state_ptr) -> version
visual_frame_version()
```

JS 侧使用持久 `ImageData`，每帧把 WASM RGBA 复制到 Canvas：

```js
const view = new Uint8ClampedArray(core.HEAPU8.buffer, framePtr, w * h * 4);
const image = new ImageData(view, w, h);
ctx.putImageData(image, 0, 0);
```

然后用固定 target scale 和 CSS 展示。这样 JS 不再执行 `fillRect`、`stroke`、`drawImage` 来解释原版 scene。`wasm/screen.js` 保留为可选的 debug inspector 可以，但不应是正式渲染器。

WASM audio ABI、PAK 解析、解码和 spectrum ABI 不变。新增 visual API 只消费已经打包好的 611 float 和状态字段。

## 分阶段迁移

### Phase 0：建立基线，不改正式画面

- 新增 deterministic fixture 和 `VisualState` schema。
- 给 Windows 增加 offscreen frame capture。
- 给 WASM 增加 raw RGBA 读取和固定 test viewport。
- 固定 ORIGINAL theme、640×480、1× 和 2×。
- 采集当前 GDI PNG 和 Canvas PNG。
- 记录 `devicePixelRatio`、client size、DPI awareness、audio PTS、tick、palette/asset hash。
- 确认当前 `tests/wasm_browser.py` 所缺的 Playwright 之外，CI 有稳定的 headless browser capture。

验收：

- 同一 fixture 连续采集 10 次，Windows raw frame hash 全部相同。
- browser raw canvas hash 全部相同。
- 合成层截图 hash 在固定 DPR/viewport 下相同。
- 每个失败都能定位到 state、asset、raster 或 presentation 四层之一。

### Phase 1：抽出 scene 和只读状态

- 把 `PaintScene` 的页面分支、主题、频谱和 stage memory 读取改成 `VisualState`。
- 保留现有 GDI 绘制作为 legacy backend。
- 输出一份同顺序的 draw list，仅用于调试和对比，不先替换 Windows。
- 定义 clip stack、整数矩形、blit、文本和 alpha 的统一语义。

验收：

- 同一 `VisualState` 的 GDI 结果与 Phase 0 基线完全一致。
- scene 函数不再读写 HWND、DOM、音频对象或全局 tick。

### Phase 2：先实现静态 ORIGINAL TR 页

- 实现 software rasterizer 的 fill、integer blit、clip、atlas text。
- 只需要先覆盖背景、frame、title、spectrum field/seams/axis、info、grid、selection 静态部分。
- 字体先只做 ASCII atlas。
- 与当前 GDI 参考逐帧比对。

验收：

- 640×480 静态帧最大通道差为 0。
- 1280×960 整数倍帧最大通道差为 0。
- title、axis、selection、每个 grid cell 都有区域 hash。

### Phase 3：迁移动画和状态

- 固定 `ui_tick`、`audio_pts`、`seek_epoch`。
- 移植 frame entrance、panel fade、selection pulse/stripe、hint blink、tab fade。
- 移植 `stage::Memory::advance` 的实际时间阈值。
- 在 state adapter 中明确 pause、stop、seek、track change。
- 保持 spectrum core 不改，除非先有独立核心 golden。

验收：

- 同一 tick 序列的每个 RGBA 帧完全一致。
- 暂停、停止、seek、换曲的分支 fixture 全部通过。
- 1080p/1440p/4K 下 renderer 内部状态不随 rAF 频率改变。

### Phase 4：切换 WASM 到共享帧

- 新增 `wasm/visual_api.cpp`，返回 RGBA 指针。
- `screen.js` 改为 `putImageData`，删除手绘 scene。
- 固定 Canvas backing store 和 CSS target scale。
- 用 `devicePixelContentBoxSize` 验证实际设备像素。
- 禁止非整数缩放进入 parity test。

验收：

- native 和 WASM 的 raw RGBA hash 相同。
- DPR 1/1.25/1.5/2/3 下整数物理倍率呈现无混合色。
- 同一 audio PTS 的截图无画面内容差异。

### Phase 5：扩展 stage、主题、LIB/SYS 和 CJK

- 先迁移 stage 和 wing，再迁移 theme。
- LIB、SYS 和文本输入单独设边界。
- 如果 CJK 不能共享字体栅格器，就把搜索输入和动态 metadata 明确排除在 pixel-parity 画布之外，或构建共享 glyph atlas。
- `web/` 保持实验入口，不把 CSS 版本接入 golden。

验收：

- 每个 F7/F8 组合和五个主题都有 fixture。
- 所有动态文本都有按照重绘规则和字体策略定义的 golden 或明确例外。
- 1:1 声明只覆盖已通过的区域，不用“主视觉相似”替代。

## 1:1 的边界

在这些条件下，才可以说“WASM 与 Windows 视觉 1:1”：

- 相同 `VisualState`。
- 相同 palette、atlas、glyph 和 renderer 版本。
- 相同 target surface 尺寸和整数 scale。
- 相同 audio PTS、tick 和 seek epoch。
- 相同 alpha/blend 规则和 RGBA 字节序。
- golden 比较的是 raw frame，不是带窗口边框的桌面截图。

即使满足这些条件，以下仍然不是 1:1 的保证：

- Windows 和浏览器在任意窗口尺寸/任意 DPR 下的合成时机。
- 不同音频设备输出 latency 下的肉眼同步。
- 用户系统色彩管理、截图工具和视频编码。
- 浏览器原生输入控件、字体 fallback、无障碍层和系统菜单。
- 非整数缩放、旋转、缩放动画和 GPU 后处理。

建议把目标表述成：

> 在固定 ORIGINAL theme、固定 640×480 canonical surface、固定整数物理倍率、固定 audio PTS 和 tick 的 reference mode 下，Windows 与 WASM 的 raw frame 逐字节一致；正常响应式布局仅承诺同一 canonical frame 的几何适配，不承诺任意 DPR 的逐字节一致。

这不是降级，而是把可验证的承诺和不可控的平台合成分开。

## 仍存风险

1. **现有 Windows 基线本身不是 scale-invariant。** 部分 pen 宽度和 GDI 字体按物理像素栅格化，2× 画面不一定是 1× 的整数复制。必须先决定 canonical scale。
2. **DPI awareness 未定义。** 没有 manifest 或 `WM_DPICHANGED` 时，系统缩放会改变客户区、字体和截图含义。测试必须在明确的 DPI 模式中运行。
3. **CJK 字体是最终难点。** GDI `NONANTIALIASED_QUALITY` 的 hinting 不适合直接交给浏览器字体。全量 atlas 会增加资源和许可审查成本。
4. **Alpha 舍入和 GDI 扫描转换需要实测。** `AlphaBlend`、stock pen、ellipse 和 polygon 在不同 Windows 版本/驱动上可能有边界差异，不能只凭 API 名称推断。
5. **pause 当前存在实现分歧。** Windows 频谱继续平滑，WASM 清零，Web idle。必须选择“复刻现状”还是“按 roadmap 冻结”，两者不能同时叫同一个 baseline。
6. **seek 的旧状态没有统一定义。** 当前 C++ 频谱不会在 seek 时先清空，stage 只在位置变化超过阈值时丢历史。golden 需要把该细节固定下来。
7. **浏览器合成不是可移植的像素契约。** 即使 `toDataURL()` 一致，最终屏幕在 fractional DPR 下仍可能不一样。必须额外测试 presented frame。
8. **性能需要实测。** 640×480 RGBA 很小，但 2×/3× 和每帧 `putImageData` 的拷贝、浏览器 GPU 上传、off-main-thread 处理都要测长曲和低端 Android。
9. **现有 `web/` 不能直接晋级。** 它的状态、时钟、字体和频谱语义与 core 不同，改成正式主线会把两个平台都带偏。
10. **测试依赖尚未对齐。** 仓库文档要求 Playwright，但当前环境没有安装；本研究的浏览器采样使用了本机 Edge 和临时 CDP 探针，正式 CI 需要锁定浏览器版本和启动参数。

## 已执行的验证

- 确认工作目录为指定 worktree，分支为 `research/wasm-visual-parity`，HEAD 为 `56cd7134bd6a77b1ceb231bf60b2b47319b01ae8`。
- 阅读了本任务要求的源码、文档、资产生成脚本和测试。
- `cmake -S . -B obj/core -DBUILD_TESTING=ON`、构建和 `ctest` 通过，`pak_index`、`audio_core` 两项测试通过。
- 使用本机 headless Edge 对 `wasm/` 做了固定 fixture 采样：
  - 内部 Canvas 始终为 640×480。
  - DPR 1/2 只改变合成，不改变 raw buffer。
  - 390、640、850、1280 viewport 的 CSS 尺寸分别为约 378、511.984、639.969、818，均不保证整数像素映射。
  - 固定 fixture 的 640×480 raw canvas 可以稳定输出 PNG 和 SHA-256。
- 对 `docs/screenshot-original.png` 与当前 Canvas 的静态区域做了最近邻 2× 到 1× 对照。差异主要集中在 title run、selection 和由不同资产路径产生的边缘色；该对照只作为研究探针，不作为正式 golden。

正式实现时，建议先提交 Phase 0 的 capture/fixture，再提交 Phase 2 的静态 renderer。不要先把 `wasm/screen.js` 继续手工补全，因为每补一块都会增加未来移除的代码和视觉“看似接近”的错误基线。
