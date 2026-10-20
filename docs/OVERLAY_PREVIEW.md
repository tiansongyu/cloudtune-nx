# 覆盖层桌面预览

此工具在 Linux 桌面运行同一份 `overlay/source/main.cpp`，用小型 SDL2
适配层提供 Tesla 绘制和输入接口。独立测试进程复用项目的音频引擎，
通过 Unix socket 接收播放命令。预览窗口关闭后，测试进程继续运行。

这是开发工具：可以检查中文、黑胶动画、播放控制和进程分离；Tesla 合成、
Atmosphère CMIF、游戏混音和休眠仍需 [实机验收](HARDWARE_TESTING.md)。
预览没有加载 Switch `.ovl`，也不宣称与 Tesla 的布局逐像素一致。

## 启动

需要 [宿主构建依赖](BUILDING.md)、SDL2、Python 3 和 FFmpeg。
无需 SDL2_ttf，也无需登录。首次运行会构建两个原生可执行文件，
并生成两段原创合成音频；不会读取主应用的账号或播放队列。

```bash
python3 scripts/overlay-preview.py
```

默认使用当前 `DISPLAY`。后台回归可以另启 Xvfb，再指定其显示编号；
脚本不会自行切换桌面或操作现有的模拟器窗口。

初始队列暂停，音量为 **0%**。按 **X** 开始播放，黑胶随播放旋转；
选中「音量」后按右键调大音量。右侧为桌面说明区，左侧为覆盖层界面。

| 输入 | 行为 |
| --- | --- |
| 上 / 下 | 移动列表焦点 |
| Enter / A、鼠标单击 | 确认当前项目 |
| 鼠标滚轮 | 滚动列表 |
| X | 播放 / 暂停 |
| L / R | 上一首 / 下一首 |
| 左 / 右 | 选中音量行时，每次调整 5% |
| Escape、关闭窗口 | 关闭预览，保留独立播放器 |

```bash
# 构建后快速重开；--detach 让终端立即返回
python3 scripts/overlay-preview.py --no-build --detach

# 查看真实播放进程状态；命令成功表示入队，状态稍后更新
python3 scripts/overlay-preview.py --command status
python3 scripts/overlay-preview.py --command play
python3 scripts/overlay-preview.py --command volume --argument 100

# 关闭本预览窗口及其独立播放器
python3 scripts/overlay-preview.py --stop
```

## 目录与组件

| 路径 | 内容 |
| --- | --- |
| `overlay/preview/` | 桌面绘制适配、客户端和测试传输协议 |
| `overlay/source/main.cpp` | 与实机覆盖层共用的 UI 源码 |
| `background/tests/overlay_preview_service.cpp` | 独立宿主音频测试服务 |
| `build/overlay-preview/` | CMake 缓存、原生程序与中间文件 |
| `.runtime/overlay-preview/` | 私有测试队列、生成音频、socket、进程记录及日志 |
| `build/validation/` | 本机测试日志和截图 |

使用 `--directory PATH` 可另建一套隔离数据。目录权限为 `0700`，
socket 为 `0600`。测试队列仅允许已有的本地音频文件。构建产物及运行
数据均被 Git 忽略，也不会加入 Switch SD 发布包。

原生预览默认不参与构建，可显式开启：

```bash
cmake -S . -B build/overlay-preview -G Ninja \
  -DPLATFORM_DESKTOP=ON -DCLOUDTUNE_BUILD_OVERLAY_PREVIEW=ON
cmake --build build/overlay-preview --target \
  cloudtune-overlay-preview cloudtune-preview-service
ctest --test-dir build/overlay-preview --output-on-failure \
  -R '^overlay-preview-service$'
```

该测试覆盖独立进程播放、暂停、切歌、进度、音量、循环、随机、
断线续播、协议异常、单实例和退出清理。GitHub Actions 的宿主任务也构建
两个预览程序并运行此回归；CI 的音频设备为 SDL dummy，不代表声卡听感验证。

GUI 与引擎保持两个可执行文件。前者遵循覆盖层的 GPL-2.0-only，
后者为 GPL-3.0-or-later，共享协议为 GPL-2.0-or-later。

## Eden 0.2.1 的实际 OVL 测试

2026-10-20，在独立 Xvfb 桌面和匿名配置中，将已构建的 `CloudTune.ovl`
原样复制为 `.nro` 供 Eden 加载。Eden 启动该程序，但在首帧前退出：
`hid:sys` 的 HOME / SLEEP 事件句柄接口未实现，Tesla 随后等待无效句柄，
得到 `0xE401 (2001-0114)`。

测试文件 SHA-256：
`918b37b3385947ea27203819816ae0f2f3707b14d99e4961981e976f03c3df87`。
本机证据为 `build/validation/eden-overlay-test-plain.log` 与
`build/validation/eden-overlay-test.png`。后者是黑屏失败证据。
该结果仅描述本次 Eden 配置；桌面预览作为独立的开发验证路径。
