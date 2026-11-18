# SOUND MEMOIRS Web Prototype（探索支线 · 无需同步）

> **状态：纯 JS 视觉探索支线，无需与桌面主线同步。**
>
> 本目录是独立的原型，**不参与一比一复刻，也不需要跟随 `src/` 或 `wasm/` 的任何改动**。
> 它与桌面端不共用绘制代码，两套主题（`INSTRUMENT` / `PAPER`）仅是视觉比较用。
>
> 一比一复刻主线是 [TR / WASM](../wasm/README.md)：那里与桌面端共用同一套 C++ 绘制与算法代码
> （`src/scene.cpp` + `src/gfx.h` 双后端），任何画面或交互改动只需同步主线。

一个零构建、零外部服务的本地音乐播放器原型。页面包含两套主题：

- `INSTRUMENT`：基于原版 640×480 `SOUND MEMOIRS` 画面的仪器布局。
- `PAPER`：纸色档案页、装订孔与横向记录线组成的收藏档案风格。

## 运行

直接打开 `index.html`，或在本目录启动任意静态文件服务器：

```powershell
python -m http.server 4173
```

然后访问 `http://127.0.0.1:4173/`。

## 播放器功能

- 本地多选、拖放文件或拖放整个文件夹；支持的格式取决于当前浏览器，通常包括
  MP3、OGG、WAV、FLAC、M4A 和 Opus。
- 播放/暂停、上一首、下一首、随机、关闭/列表/单曲循环、搜索和曲目移除。
- 43 段 Web Audio 频谱、时域波形与左右电平表。
- 进度和音量都使用 Pointer Events，鼠标、触控笔和触摸拖动走同一套逻辑。
- 读取 MP3 ID3v2 与 FLAC Vorbis Comment 中的标题、艺人、专辑和可选封面。

## 本地与隐私

选中的音频通过 `File` 与 Blob URL 直接交给当前页面的 `<audio>` 元素和 Web Audio API。
页面不上传文件，不发起音频网络请求，也不会把本地曲库写入持久化存储。主题、音量、
循环和随机状态仅保存在 `localStorage` 中。
