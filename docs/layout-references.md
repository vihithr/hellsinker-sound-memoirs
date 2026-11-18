# 画面布局：设计参考与依据

本文记录 Windows 播放器新增四种画面布局（`FIELD` / `STACK` / `RING` / `CROSS`）时所依据的外部设计资料、代码事实与验收数据。目标是让后来者不必回溯对话，也能判断每条设计决定从哪里来、哪些是可改的偏好、哪些是不可动的约束。

阅读前需要知道的前提：

- 播放器以 640×480 逻辑坐标绘制，再按窗口等比放大并居中留边（`src/ui.cpp` 的 `EnsureBackBuffer`）。
- `ORIGINAL` 布局是对 HellSinker 原作 `SOUND MEMOIRS` 画面的复刻，其几何常量是从原版画面测量得到的，不是设计选择。
- 本轮新增的四种布局属于二次创作，外部资料只用来校准取舍方向；具体坐标全部是本仓库内决定的，没有从任何外部项目拷贝数值。

## 1. 不可动的约束

| 约束 | 来源 | 影响 |
|---|---|---|
| `ORIGINAL` 布局与演出必须逐像素不变 | 用户明确要求 | 所有共享绘制函数改为从几何表取值，几何表的 Original 项逐字段等于旧常量 |
| 不改 `stage.cpp`、`core/`、音频链路 | 控制改动面 | 新布局直接读 `memoirs::Spectrum` 与 `stage::Memory`，不调用 `stage::paint` |
| 640×480 逻辑画布与最近邻放大 | 既有实现 | 新布局的线宽用 `max(1, S(1))`，避免 1× 下消失 |
| 新布局只接管 `TR` 页 | 见 §4.4 | `LIB` / `SYS` 保持原版坐标，命中区域稳定 |

## 2. 外部设计参考

以下资料用于确认“这类构图在别处是否成立、成熟做法是什么”。它们只影响方向判断，不提供代码或数值。

| 参考 | 与本文相关的结论 | 采用情况 |
|---|---|---|
| [Engineering Blueprint UI](https://uidesignprompts.com/prompts/engineering-blueprint-ui) | 技术制图风格由细栅格、标注引线、尺寸线构成；栅格可见但不主导 | 部分采用：`STACK` 用了边框与刻度；未做引线标注 |
| [User Interfaces of the Demo Scene](https://www.datagubbe.se/scenegui/) | 用固定字符网格当版式骨架是成熟做法，信息密度可以很高 | 采用：`FIELD` / `CROSS` 的 `TAB` 网格直接复用原版 8px 字格 |
| [The Effect of CRTs on Pixel Art](https://datagubbe.se/crt/) | 扫描线、晕影只在足够放大倍率下成立，否则破坏可读性 | 未采用：GDI 无 shader，且 1× 会糊 |
| [Fixing Pixel Art（Shadertoy）](https://www.shadertoy.com/view/XsjSzR) | CRT 后处理是逐像素管线 | 未采用，理由同上 |
| [Keysight：示波器 XY 模式](https://www.keysight.com/used/us/en/knowledge/glossary/oscilloscopes/xy-mode-oscilloscope) | XY 模式显示两个通道的相位关系，单声道退化为一条斜线 | 采用：`RING` / `CROSS` 中心窗改成 L/R 直接作图 |
| [EDN：Using oscilloscope X-Y displays](https://www.edn.com/using-oscilloscope-x-y-displays/) | XY 图形靠轨迹本身传达相位，不需要额外装饰 | 采用：中心窗只保留菱形边框与轨迹 |
| [Pen Plotter Programming](https://medium.com/@fogleman/pen-plotter-programming-the-basics-ec0407ab5929) | 绘图仪靠线宽与间距表达密度，不靠色阶 | 部分采用：`STACK` 的波形与柱体都用纯色块 |
| [VU meter](https://en.wikipedia.org/wiki/VU_meter) | 指针表有明确的上升时间与超调规范（300 ms 到 99%） | 未采用：本轮布局未引入新的包络 |
| [Practical VU Meter Circuits](https://michaelfidler.com/articles/practical-vu-meter-circuits) | 同上，含刻度与过载区惯例 | 未采用 |
| [8-Point Grid](https://gridmakerpro.com/grids/typography-grids/8pt-spacing-grid) | 间距取 4 / 8 的倍数可减少随意感 | 部分采用：新布局的行高取 6 / 17 / 18，未强制 8 的倍数 |
| [Font Rendering: Respecting the Pixel Grid](https://blog.codinghorror.com/font-rendering-respecting-the-pixel-grid) | 像素字必须落在整数网格上 | 采用：字格 8×11，`S()` 后仍为整数倍 |
| [Pixelblog 26：UX/UI Basics](https://www.slynyrd.com/blog/2020/2/23/pixelblog-26-uxui-design-basics) | 像素 UI 优先保证对比与可读，而非装饰 | 采用：所有信息行保持高对比 |
| [Dithering in Pixel Art](https://motifpad.com/guides/dithering-pixel-art) | 小于 8–10px 的区域不应抖动 | 采用：新布局不使用抖动 |
| [projectM](https://github.com/projectM-visualizer/projectm) / [MilkDrop](https://en.wikipedia.org/wiki/MilkDrop) | 全屏迷幻可视化以画面为主体，信息退居其次 | 未采用：与“字格 + 曲目区必须可读”冲突 |

外部资料检索日期：2026-11-18。这些页面是二手描述，本文只引用其结论方向，未引用其中任何代码或数值。

## 3. 代码依据

新布局的版式全部落在既有实现之上，关键事实如下。

| 事实 | 位置 |
|---|---|
| 逻辑画布 640×480，`S()` 负责缩放，`g_scale` / `g_offX` / `g_offY` 负责居中 | `src/ui.cpp` 顶部常量与 `S()` |
| 原版几何常量：网格 `168,296`，列宽 56、行高 16，5 列 10 行；信息行 `220,256/264`；提示 `144,472`；通道栏 `366`，宽 40；进度条 `154..486 / 266` | `src/layout.cpp` 的 `kOriginalTracks` |
| 频谱窗 `148..492 / 102..236`，43 段，段宽 7、步距 8，基线 232 | `src/layout.cpp` 的 `kOriginalSpec` |
| 字格为 8×11 位图，`DrawCells` 按 8px 步进，最近邻拉伸 | `src/ui.cpp` 的 `DrawCells` |
| 主题为 21 个颜色角色，由 `assets/palettes.json` 生成 | `src/themes.generated.h` |
| 演出状态（低频持续量、瞬态、左右包络、历史脊线、轨迹） | `src/stage.h` 的 `stage::Memory` |
| `PaintScene` 是唯一的整帧入口，按页面与布局分派 | `src/ui.cpp` 的 `PaintScene` |
| 设置面板为固定坐标，行数由 `kSettingCount` 驱动 | `src/config_panel.h` / `.cpp` |
| 构建脚本显式列出源文件 | `build.bat` 第 14 行 |

原版数值来源：早期逆向阶段对原版画面逐帧测量（见 `reverse/` 下的交接材料），本轮没有重新测量，也没有改动这些数值。

## 4. 设计决策与理由

### 4.1 布局是与演出、侧翼正交的第三个维度

`F7` 决定中央演出的算法，`F8` 决定左右侧翼，布局决定整块画面如何分区。三者组合会迅速膨胀，因此本轮让布局“自带全套视觉”，新布局不读取 `F7` / `F8`。代价是组合能力有限，收益是每种布局都能独立调到可交付状态。

### 4.2 四种布局的分区依据

| 布局 | 分区 | 依据 |
|---|---|---|
| `FIELD` | 无边框，基线 y=340，43 段横贯全宽，上下镜像 46px | 频率轴是唯一必须保留的信息；去掉面板框后，横向空间可以直接给数据 |
| `STACK` | 24px 标题条 + 左 8..88、中 96..400、右 408..632 | 46 首曲目需要常显，三栏是能在 640 宽内容纳 2×23 行的最省空间方案 |
| `RING` | 圆心 (320,236)，频段 132..198，曲目刻度半径 206 | 43 段在圆周上每段约 8.4°，1× 下仍可分辨；把曲目放到外圈避免与频段争空间 |
| `CROSS` | 中心 (320,240)，四轴到 24/616/24/456 | 十字分区让“标题、进度、左右声道”各占一轴，中心留给 XY 窗 |

### 4.3 中心窗使用 XY 而不是历史轨迹

第一版中心窗画的是 `stage::Memory::traces`（即 `L-R` 与 `L+R` 的旋转坐标），截图里呈线团状，可读性差。改为 `waveL` / `waveR` 直接作图后，单声道退化为一条线、宽立体声才展开，与示波器 XY 模式的预期一致。这一改动来自 [Keysight](https://www.keysight.com/used/us/en/knowledge/glossary/oscilloscopes/xy-mode-oscilloscope) 与 [EDN](https://www.edn.com/using-oscilloscope-x-y-displays/) 对 XY 显示的描述。

### 4.4 新布局只接管 `TR` 页

`LIB` 与 `SYS` 是功能页，搜索框、进度条、设置行都有固定命中区域。若把布局坐标套到这两页，需要同时改写 `archivePaint`、`config_panel` 与全部鼠标命中，收益低、风险高。因此切到 `F2` / `F3` 时回到原版面板坐标，返回 `TR` 时恢复布局。这一取舍写入 `USAGE.md` 的“已知限制”。

### 4.5 `TAB` 网格复用原版坐标

`FIELD` 与 `CROSS` 没有常显曲目列表。呼出的网格直接使用 `168,296` 的原版几何，因此选中框、EXIT 条与命中测试都无需新增逻辑。这是“不发明第二套网格”的直接结果。

## 5. 未采用的方案

| 方案 | 不采用的理由 |
|---|---|
| 全屏 Milkdrop 式可视化 | 与“字格 + 曲目区必须可读”的前提冲突，且需要逐像素后处理 |
| 扫描线 / 晕影叠加层 | GDI 无 shader；1× 下会破坏可读性 |
| 窄区域抖动 | 小于 8px 的区域会变糊，见 §2 的抖动规则 |
| 粒子或装饰性叠层 | 只增加亮度不增加信息，评估标准是“重拍是否更清楚” |
| 让 `RING` / `CROSS` 叠加 `F7` 演出 | 演出按 148..492 / 80..238 的固定区域绘制，套进环形或十字需要重写演出接口，超出本轮范围 |

## 6. 验收证据

| 检查 | 方法 | 结果 |
|---|---|---|
| 原版零回归 | 改动前后各用 `--shot` 渲染 5 套主题的 `Layout=0`，逐像素求差 | 5/5 `diff bbox = None` |
| 真实 pak 解码 | `hsbgm.exe --selftest` | 46/46 解析、前三首解码、`PASS` |
| 布局矩阵 | 5 布局 × 5 主题，2× 与 1× 各一轮 | 全部生成，无裁切、无越界 |
| 配置读写 | ini 写 `Layout=3` 后不传 `--layout` 截图 | 输出与 `RING` 完全一致 |
| 截图副作用 | `--shot` 前后对比 ini | 不被改写 |
| 启动 | GUI 启动后存活 3 秒 | 未退出、未崩溃 |

复核命令：

```bat
build.bat
dist\hsbgm.exe --selftest
dist\hsbgm.exe --shot out.bmp --theme 0 --layout 0 --stage 0
dist\hsbgm.exe --shot ring.bmp --theme 0 --layout 3 --stage 0
dist\hsbgm.exe --shot one.bmp --theme 1 --layout 2 --shot-scale 1
```

`--shot` 对新布局注入一组确定性合成频谱，因此截图可复现；`Layout=0` 不注入，保持静音基线。

## 7. 可能被误读的地方

- 本文的外链是设计方向的佐证，不是实现来源。新布局没有任何代码或数值来自这些项目。
- `FIELD` 的镜像下摆、`STACK` 的栏宽、`RING` 的半径都是本仓库的取值，可以调整，不构成约束。
- `ORIGINAL` 的几何常量是复刻基准，改动它们等于改动“原版复刻”，属于另一类变更。
