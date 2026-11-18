# SOUND MEMOIRS — 静态部署包

纯静态交付物：HTML + CSS + JS + WebAssembly + PNG，无后端、无数据库、无服务端 API。
浏览器端与桌面端共用同一套 C++ 绘制代码（`src/scene.cpp` + `src/gfx.h` 双后端）。

## 部署到 GitHub Pages（推荐）

本目录就是可直接发布的站点内容。

1. 在 GitHub 上新建一个仓库（例如 `hsbgm-web`）。
2. 把本目录的**内容**（`index.html`、`app.js`、`scene.js`、`worker.js`、`screen.css`、`core/`、`assets/`）提交到仓库**根目录**。不要多套一层子文件夹。
3. 仓库 Settings → Pages → Build and deployment：Source 选 `Deploy from a branch`，Branch 选 `main` / `/ (root)`，Save。
4. 等待部署完成，访问 `https://<用户名>.github.io/<仓库名>/`。

**关于子路径**：所有资源引用都是相对路径（`./core/core.js` 等），所以项目站点带 `/仓库名/` 前缀也能正常工作，无需改代码。若你有自定义域名并用 `CNAME`，同样可用。

**注意 Jekyll**：GitHub Pages 默认用 Jekyll 构建。本目录没有以 `_` 开头的文件，不会被 Jekyll 排除。若想彻底跳过 Jekyll，在仓库根目录放一个空文件 `.nojekyll`（可选，但推荐）。

## 本地验证

```powershell
cd dist-static
python -m http.server 8080
```

打开 `http://127.0.0.1:8080/`。**不要双击 `index.html`**：ES 模块和 Worker 在 `file://` 下会被浏览器拦截，画布会全白。

此前把核心模块命名为 `.mjs` 时，Python 的默认 HTTP 服务会返回 `text/plain` 导致白屏；现已统一为 `core.js`，`python -m http.server` 可直接用。GitHub Pages 对 `.js` / `.wasm` 的 MIME 也都能正确处理。

## 使用

页面就是纯画面：黑底上居中一块 640×480 原版画面，等比放大到当前窗口大小，没有网页控件和假窗口装饰。浏览器标签就是「窗口」，VOL / 主题显示在标签标题上。所有交互都在画面内用键盘或鼠标完成。

**首次使用**：按 `Ctrl+O` 或拖放文件到画面，选择游戏 `backboneteller` 目录下的 `bbt.pak` / `bbt2.pak` / `bbt3.pak`（有 `bbll.pak` 也可一并选），或直接选 WAV / OGG 单曲。文件通过 File API 本地读取，不上传。

| 键 | 功能 |
|---|---|
| ↑ ↓ ← → | 5×10 曲目网格移动 |
| Enter / Space | 播放选中曲；正在播放时暂停/继续；EXIT 上停止 |
| S / Esc | 停止 |
| + / - | 音量 ±5 |
| C / T | 切换主题：ORIGINAL → DARK → NEON → PAPER → BLUEPRINT |
| N / P、PgDn / PgUp | 本地播放列表下一首 / 上一首 |
| Ctrl+O | 打开文件选择 |
| Ctrl+Q | 停止 |
| F7 / F8 / F9 | 切换演出 / 侧翼 / 画面布局 |
| Tab | 在 FIELD / CROSS 布局中呼出原版网格 |
| 鼠标单击 / 双击 | 选中 / 播放（EXIT 双击停止） |
| 拖放文件 | 多文件入播放列表 |
| F1 / F2 / F3 | 切换 TR / LIB / SYS 页 |
| Alt+Enter | 切换全屏（画面右下角辅助按钮也可，静止后自动淡出） |

## 内容边界

已复刻：Original 及 FIELD/STACK/RING/CROSS 布局、4 种 stage 演出、5 种主题、43 段频谱、电平、波形、标题与频率轴、入场/闪烁/选中脉冲动画，以及 PAK/WAV/OGG 解码与 125ms 视觉游标补偿。

未包含：搜索框内的 ID3 元数据（艺人/专辑）完整解析、系统曲库持久化、MP3 解码、流式解码（单曲上限 96 MiB）。

## 体积

约 200 KiB（未压缩），其中 `core.wasm` 166 KiB。`core.js` / `core.wasm` 内容固定，可设长期不可变缓存。

验证状态：24 个场景帧通过；与桌面 `hsbgm.exe --shot --layout 0` 基准对照，平均像素差 0.0006（307200 像素中 2 个不同）。
