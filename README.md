# LyricSync · DAW 歌词同步显示插件（VST3）

在音乐宿主里播放轨道时，插件窗口同步显示当前句子的歌词；**把播放头拖到任意时间码、循环、倒退，歌词都会立即跟着跳到对应句子**。

插件对音频完全直通（不改一个采样），只负责读宿主播放头 + 显示歌词。

---

## 一、为什么跳转不会错位

常见的"歌词滚动"做法是插件自己计时（每处理一个 buffer 就累加时间），一旦跳转、循环或宿主停止，内部时间就与工程对不上。

LyricSync 不这么做：

```
每个音频回调  →  向宿主读取 ProcessContext.projectTimeSamples / sampleRate
             →  得到"项目绝对时间"（秒）
             →  写入原子变量
GUI 每帧      →  读原子变量 → 对歌词时间戳二分查找 → 渲染当前句
```

也就是说，插件内部**没有任何时间累加**。播放、拖拽时间码、循环回绕、从 3 分跳回 12 秒，走的是完全相同的代码路径，因此下一帧（约 33 ms）就能对齐。

JUCE 的 `AudioPlayHead::getPosition()` 在 VST3 下读的正是 Steinberg `ProcessContext` 的 `projectTimeSamples`（以及 tempo、拍号、播放/录音/循环状态）。节拍/小节信息由 `ppqPosition` 换算，仅供显示。

---

## 二、功能

| 项目 | 说明 |
| --- | --- |
| 显示 | 上一句 / 当前句（大字高亮）/ 下一句 / 双语译文 |
| 逐字高亮 | 支持增强 LRC 的 `<mm:ss.xx>` 字级时间戳，已唱部分逐字染色 |
| 时间码 | `mm:ss.mmm`、小节·拍·tick、BPM、拍号、播放/录音/循环状态 |
| 歌词来源 | 拖入文件、文件选择、窗口内粘贴编辑、内置示例 |
| 格式 | 标准 LRC、增强(逐字) LRC、SRT/WebVTT 风格字幕、无时间戳纯文本（自动按每句 4 秒排布） |
| 编码 | UTF-8 / UTF-16 LE / GBK(GB18030) 自动识别（国内老 LRC 常见 GBK） |
| 微调 | 整体偏移 ±100 ms、归零；LRC 内 `[offset:]` 自动生效；字号滑块 |
| 手动预览 | 内置模拟播放头，可在没有宿主时间码时校对歌词、拖动定位 |
| 状态保存 | 歌词全文、偏移、字号、主题、**界面语言**、窗口尺寸随宿主工程一起保存 |
| 界面语言 | 右上角下拉框切换 **中文 / English**：按钮、提示、状态、解析警告、内置示例歌词全部同步切换；已载入的用户歌词保持原样 |
| 外观 | 深/浅两套主题；**整窗等比缩放**（固定 960×600 逻辑画布，绘制、控件、字体共用一个缩放系数，窗口保持 8:5，宿主强制改比例时自动居中留边，不会裁切按钮或进度条） |
| 附带 | Standalone 独立程序（不上 DAW 也能当提词器用） |

---

## 三、构建

### 环境要求（Windows）

- Visual Studio 2022（或 2019），勾选 **「使用 C++ 的桌面开发」** 与 **「适用于 Windows 的 C++ CMake 工具」**
- Git（用于自动下载 JUCE）
- 不需要单独下载 VST3 SDK：**JUCE 7.0.12 已内置 VST3 SDK 3.7.x**

> 工程里**不含预编译二进制**，请用下面任意一种方式生成 `.vst3`。

### 方式一：零本地环境云编译（推荐给没有 Visual Studio 的电脑）

工程已自带 `.github/workflows/build-windows.yml`，GitHub 的 Windows 运行器自带 Visual Studio 与 CMake，不需要你装任何开发工具。
工作流会**自动探测生成器**：2026 年 `windows-latest` 已换成 `windows-2025-vs2026`（只有 VS 2026 / CMake 4.x），
因此优先用 `Visual Studio 18 2026 -T v143`，再依次回退到 18 默认工具集 / VS 2022 / Ninja，不会再出现写死生成器导致的失败：

1. 浏览器打开 **<https://github.com/new>**，新建仓库：名字填 `LyricSync-VST3`，选 **Public**，
   **不要**勾选 Add a README / .gitignore / license（保持完全空仓库），点绿色 **Create repository**。
2. 复制新仓库页面上给出的地址（`https://github.com/<用户名>/LyricSync-VST3.git`）。
3. **双击本目录的 `push-to-github.bat`**。脚本已自动定位本机便携版 Git，并预填了默认仓库地址，
   **直接按回车**即可；若仓库地址不同，粘贴正确地址再回车。第一次推送若弹出浏览器登录 GitHub，点授权即可。
   （等效命令：`git remote add origin <地址>` → `git push -u origin main`；本地仓库已初始化并提交好。）
   > 国内网络常见 `Failed to connect to github.com:443`：Git 默认不读 Windows 系统代理。
   > 脚本会自动读取注册表中的系统代理（本机为 `127.0.0.1:10808`）并带上 `http.proxy` 重试，无需手工配置。
   > 若新建仓库时 GitHub 自动放了 `.github/workflows/blank.yml`（或勾了 README），首次推送会报
   > `! [rejected] main -> main (fetch first)`。这不是密码错误，只是远程多了一个初始提交。
   > 脚本会自动 `fetch` + 合并远程历史（保留本工程文件、丢弃那个无用的 CI 模板），再重新推送；
   > 万一仍失败，直接**再双击一次本脚本**即可。
4. 推送会**自动触发**编译：打开仓库 → **Actions** → **Build VST3 (Windows)** 查看进度。
   若没自动开始，点 **Run workflow** 手动触发。
5. 变绿后在该次运行页面底部 **Artifacts** 下载 `LyricSync-VST3-Windows-x64`，
   解压即得到 `LyricSync.vst3` 和 `LyricSync.exe`。

> 也可以不上命令行：在 GitHub 网页用 **Add file → Upload files** 上传工程文件，
> 但务必另外用 **Add file → Create new file** 手工创建 `.github/workflows/build-windows.yml`
> （隐藏目录在网页拖拽上传时容易被漏掉），内容照抄工程里的同名文件。

### 方式二：本机一键构建

双击 `build-windows.bat`（建议在「x64 Native Tools Command Prompt for VS 2022」中运行），脚本会：

1. 检查 CMake
2. 把 JUCE 7.0.12 浅克隆到 `third_party/JUCE`（已存在则跳过）
3. `cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DJUCE_DIR=...`
4. `cmake --build build --config Release --parallel`

产物：

```
build/LyricSync_artefacts/Release/VST3/LyricSync.vst3        ← 插件
build/LyricSync_artefacts/Release/Standalone/LyricSync.exe   ← 独立程序
```

### 手动构建 / 国内网络下载 JUCE 失败时

手动下载 <https://github.com/juce-framework/JUCE/archive/refs/tags/7.0.12.zip>，
解压并把文件夹改名成 `JUCE`，放到 `third_party/JUCE`（即 `third_party/JUCE/CMakeLists.txt` 要存在），再运行脚本。
也可以完全交给 CMake 自动拉取：

```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

> macOS / Linux：`FORMATS` 中已含 Standalone；把生成器换成 Xcode / Ninja 即可，
> VST3 在 macOS 上生成 `.vst3` bundle。

---

## 四、安装与使用

1. 把整个 **`LyricSync.vst3` 文件夹**复制到
   `C:\Program Files\Common Files\VST3`（需管理员权限）
   或 `%LOCALAPPDATA%\Programs\Common\VST3`（当前用户）。
2. 在 DAW 里重新扫描插件，按 `LyricSync` 搜索（Cubase / Nuendo / REAPER / Studio One / FL Studio / Ableton Live 等均可）。
3. **插在有人声音频的轨道、总线或 Master 上**（插件需要被宿主调用 `process` 才能拿到时间码；音频完全直通，不会改变声音）。
4. 打开插件窗口：默认带一段示例歌词。点「载入歌词」或直接把 `.lrc` 拖进窗口。
5. **界面语言**：右上角下拉框选 `中文` 或 `English`，按钮、提示、状态栏、警告与内置示例歌词会立即切换；设置随工程保存。切换语言**不会改动你自己载入的歌词文件**。
6. 播放 → 当前句高亮；**任意拖动播放头 / 循环 / 回退 → 歌词立即对齐**。
7. 若整体早了或晚了，用底部 `− / +` 每次微调 100 ms（正值＝歌词提前）。

### 独立程序（Standalone）

双击 `LyricSync.exe`，点右下角 **「手动预览」**（按下后高亮），即可用 `> / []` 和进度条模拟播放、跳转，验证歌词时间点。界面会保持 8:5 等比缩放，拖大拖小都不会裁切控件。

---

## 五、歌词格式示例

```lrc
[ti:歌名]
[ar:歌手]
[offset:0]

[00:12.34]第一句歌词
[00:16.80]第二句歌词
[00:16.80]第二句的译文          ← 同一时间戳的第二行会显示为译文
[00:21.00]这<00:21.00>句<00:21.35>逐<00:21.70>字<00:22.05>高<00:22.40>亮
```

- 时间戳支持 `[mm:ss.xx]`、`[hh:mm:ss,xx]`、一行多个时间戳。
- SRT：`00:00:12,340 --> 00:00:15,120` 后跟一行或多行文本。
- 纯文本：一行一句，自动按每句 4 秒排布（适合先写词，之后再改成 LRC）。

`examples/` 里有可直接试用的 `demo.lrc`、`demo-karaoke.lrc`（逐字）、`demo.srt`。

`preview/index.html` 是**界面预览 + 歌词校对工具**：用浏览器打开即可导入 LRC、模拟播放和任意跳转，先看效果，不用等编译。它不是插件本体。

---

## 六、常见问题

**Q：停止播放后，移动播放头歌词不动？**
部分宿主在停止状态下不再调用插件的 `process`，此时拿不到时间码。多数宿主（Cubase、REAPER、Studio One）在插了效果器的轨道上停止时仍会持续调用；若你的宿主不会，勾选界面上的「手动预览」用内置滑杆校对，或按一下播放再停。

**Q：插上去没声音 / 声音变了？**
插件不改音频数据。若完全静音，检查是否插在了被旁路的插槽，或宿主把插件放到了没有音频输出的轨道。

**Q：歌词整体早 / 晚？**
用 `− / +` 微调；若 LRC 自带 `[offset:xxx]`（毫秒，正值＝歌词整体延后），插件会自动应用。

**Q：变速 / 变拍工程？**
歌词按宿主项目时间（秒）对齐，与 LRC 的绝对时间戳一致。若工程做过整体变速导致音频与原始歌词时间轴不同，请用偏移微调，或重新导出与工程一致的 LRC。

**Q：界面文字全是 `æ¯å…` 之类的乱码？**
这是 1.0.0 早期构建的已知问题，已修复。根因不是字体、也不是系统区域设置：JUCE 的 `juce::String (const char*)` **只接受纯 ASCII**，源码里的中文 UTF-8 字节被逐字节当成单个字符，于是显示成乱码（二进制里的字节其实是对的）。现在所有界面文案统一由 `Lang::tr()` 经 `juce::String::fromUTF8()` 解码，并且 `Source/I18n.cpp` 是**纯 ASCII 的 UTF-8 转义源码**（由 `tools/gen_i18n.py` 生成），彻底不依赖编译器代码页。请重新下载新版 `.vst3` 覆盖安装。

**Q：拉大/拉小窗口后，按钮被裁掉、进度条不见了？**
早期版本用绝对像素坐标，窗口比例一变，底部按钮行和进度条就会被挤出可视区。现在整个界面绘制在一张固定的 **960×600 逻辑画布**上：背景绘制走 `AffineTransform`，控件走 `scaledRect()`，字体走 `ScaledLookAndFeel`，三者共用同一个 `uiScale = min(w/960, h/600)`。窗口默认被约束为 8:5；即使宿主强行给别的比例，画布也会等比居中，绝不会裁切。进度条（seek bar）改为在逻辑画布上自绘，任何尺寸下都可见。

**Q：按钮、下拉框里的字变成方块（□）？**
之前只有 Label 和大字歌词用了中文字体，按钮/下拉框走 JUCE 默认 sans-serif，遇到中文就缺字形。现在 `ScaledLookAndFeel` 统一给 `TextButton / ComboBox / Label / PopupMenu / Slider` 指定同一套中文 capable 字体，并把 `− ▶ ■ ♪ ⚠ · … 「」` 这类容易缺字形的符号全部换成 ASCII（`- > [] ! | ...`）。「手动预览」也改成 toggle 型 TextButton，走同一套字体。

**Q：载入的歌词文件乱码？**
文件支持 UTF-8、UTF-16 LE、GBK(GB18030) 自动识别；若仍异常，用记事本另存为 UTF-8 再载入。界面语言切换只影响插件自带文案，不会转码你载入的歌词。

**Q：想改界面文案 / 加第三种语言？**
编辑 `tools/gen_i18n.py` 里的 `TABLE`（key、中文、英文）和 `DEMO_ZH / DEMO_EN`，运行 `python tools/gen_i18n.py` 会重新生成 `Source/I18n.h` 与 `Source/I18n.cpp`，然后重新构建即可。

**Q：换电脑 / 重新打开工程歌词还在吗？**
在。歌词全文通过 `getStateInformation` 写进宿主工程。

---

## 七、源码结构

```
Source/
  PluginProcessor.h/.cpp   插件主体：读宿主播放头、直通音频、工程状态保存
  PluginEditor.h/.cpp      界面：三行歌词、逐字高亮、等比缩放画布、统一中文字体、拖放、手动预览
  LyricsDocument.h/.cpp    LRC/增强LRC/SRT/TXT 解析 + 二分查找
  I18n.h/.cpp              中/英双语字符串表（自动生成，纯 ASCII UTF-8 转义）
  UiPalette.h              深/浅配色
  DemoLyrics.h             内置示例歌词（按当前语言取用）
tools/gen_i18n.py          双语文案生成器：改文案后运行它再重新构建
CMakeLists.txt             JUCE CMake 工程（VST3 + Standalone）
build-windows.bat          一键构建
examples/                  示例歌词
preview/index.html         浏览器版界面预览 / 歌词校对
```

## 许可说明

JUCE 7 采用 GPLv3 / 商业双许可，本工程按 JUCE 的许可条款使用；若要闭源分发插件，请自行确认 JUCE 与 VST3 SDK 的许可要求。本工程自身代码可自由修改用于个人项目。
