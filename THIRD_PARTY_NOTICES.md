# 第三方组件与素材声明

本仓库的自有代码以 MIT 授权（见 `LICENSE`）。以下内容另有来源与许可。

## 代码

### stb_vorbis.c

OGG Vorbis 解码器，随仓库以源码形式提供（`stb_vorbis.c`）。

- 作者：Sean Barrett 及 stb 贡献者
- 许可：Public Domain（Unlicense）或 MIT，二选一

## 字体（`assets/fonts/`）

字形来自社区 HellSinker 字体包，按原包说明保留其授权：

| 文件 | 说明 | 许可 |
|---|---|---|
| `Hellsinker Main.otf` | 标题、数字等主界面用字 | CC0 / 自由使用 |
| `hellsinker_v8.ttf` | 较小的通用界面文字 | CC0 / 自由使用 |
| `unnamed771.ttf` | 基于 EXECUTOR 名上 morse 风格字形的字体 | CC BY-SA 3.0，作者 TOMOHiRO |
| `hellsinker.ttf`、`hellsinker-mono*.otf` | 上述字体的回退版本 | CC BY-SA 3.0，Christianisb 修改，原字由 Nephilim 制作 |

CC BY-SA 3.0：<https://creativecommons.org/licenses/by-sa/3.0/>

## 界面图集（`assets/*.bmp`、`assets/themed/*.bmp`）

`memoirs-cells.bmp`、`memoirs-title.bmp`、`memoirs-axis.bmp` 及其按主题重映射得到的
`themed/cells_<i>.bmp` 是从原作界面中提取 / 重排的像素字形图集，用于复刻 SOUND MEMOIRS
界面的排版与配色。

这些图形元素的版权归原作权利方所有。本仓库仅将其用于界面复刻的互操作目的，
不主张任何权利；如权利方有异议，请提 issue，会立即移除。

## 游戏音频

**仓库不包含任何游戏音频或美术资源本体。** 程序运行时从用户本机已安装的
HellSinker 目录读取 `backboneteller\bbt*.pak`，不做分发。
