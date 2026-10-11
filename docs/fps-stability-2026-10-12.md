# 60 FPS 稳定性对照记录（2026-10-12）

本轮仍未验证持续稳定 60 FPS。请求更短捕获间隔、增大 SCK 表面池、切换 BGRA、降低捕获分辨率或使用全屏测试窗口，都没有得到足以推广到生产的证据。测试后已恢复原签名主机服务；生产保持原捕获间隔、queueDepth 3 和 NV12，保留 NullMoth **1.9.0** 与 RTX 3060 H.264 硬编码。

本轮只增加诊断探针和记录，没有修改驱动。以下数据不能直接归因于驱动，也不能当作不同设置的干净因果比较。前轮调度优化和统计口径见[低延迟优化记录](latency-optimization-2026-10-11.md)。

## 环境与测量边界

- Mac：Intel macOS 虚拟机，macOS 15.8.1，RTX 3060 12 GB；源显示模式保持 **1920×1080 @ 60 Hz**。
- Windows：Radeon RX Vega，Media Foundation / D3D11 硬件视频路径，使用此前已验证的高精度 worker timer。生产链路为 ScreenCaptureKit → VideoToolbox H.264 → QUIC → Windows 解码 / 呈现。
- 动画源：自有窗口的自主 Core Animation 动画，DisplayLink 请求 60 Hz；有效窗口内回调与 presentation-layer 位置变化约 59.9 次 / 秒。这是调度与动画状态的近似观察，**不是 59.9 张实际捕获图像或光学刷新证明**。
- Safari / WebKit 等用户应用保留运行，CPU 负载随时间变化；这组测试不是严格空闲基准。
- 客户端 FPS 是成功 `Present` 的频率，不是显示器扫描测量。它与 SCK Complete、本地发送接受分别统计，不能混为一个计数。

## 生产链路对照

Windows smoke 取 **第 10–30 秒**内的客户端 FPS 样本，避开启动和动画退出尾段。表中最小值 / 中位数 / 最大值来自这个窗口，不是整段平均 FPS。捕获设置按实验顺序变化，其余编码配置保持原值。

| 测试条件 | 最小 FPS | 中位 FPS | 最大 FPS |
| --- | ---: | ---: | ---: |
| capture 60，queueDepth 3 | 44.97 | 50.95 | 53.96 |
| capture 120，queueDepth 3 | 37.98 | 51.97 | 56.99 |
| 回到 capture 60，queueDepth 3 | 32.99 | 44.47 | 52.93 |
| capture 60，queueDepth 5 | 34.98 | 43.47 | 50.99 |
| 自有窗口全屏，capture 60，queueDepth 3 | 30.96 | 40.98 | 46.98 |
| 自有窗口全屏，capture 120，queueDepth 3 | 31.96 | 38.97 | 48.98 |

“capture 120”指 `minimumFrameInterval=1/120`，编码器仍请求 60 FPS。第一次 60 → 120 → 60 回到基线后结果已有明显变化，说明源和负载未受严格控制，不能用 50.95 → 51.97 的中位数宣布改善。queueDepth 5 与全屏也没有形成可复现的稳定 60 FPS 结果，因此全部实验参数不保留。

Apple 将 minimumFrameInterval 定义为输出频率限制；请求值不保证实际交付，也不突破源原生频率。queueDepth 是服务端表面池容量，更多表面可能减少停顿，同时存在内存和延迟代价。[Apple 官方说明](https://developer.apple.com/videos/play/wwdc2022/10155/)

## 独立 SCK 对照：移除编码和像素复制

[capture-cadence.mm](../apps/macos-host/probes/capture-cadence.mm) 只记录帧状态、时间戳间隔及图像尺寸 / 格式。它不编码、不锁定或复制像素、不持有图像 surface、不采集音频、不注入输入，也不导出屏幕内容。SCK 自身的捕获与格式转换仍然存在，不能把它称作“零图形开销”。

所有下列独立测试均为 queueDepth 3、无烘焙 cursor、无音频。Complete 频率采用**整段约 20 秒实际捕获时长**，即 JSON 中 `complete_per_capture_second`；它与前表客户端第 10–30 秒的统计不是同一窗口。

| 顺序 / 独立捕获设置 | Complete / 秒 |
| --- | ---: |
| NV12，capture 60，原生尺寸 | 51.84 |
| NV12，capture 120，原生尺寸 | 54.86 |
| BGRA，capture 60，原生尺寸 | 53.93 |
| 再回 NV12，capture 60，原生尺寸 | 50.93 |

随后独立进行分辨率对照，固定 NV12 / capture 60 / queueDepth 3：

| 顺序 / 输出尺寸 | Complete / 秒 |
| --- | ---: |
| 原生 1920×1080 | 51.12 |
| 限高 720，实际 1280×720 | 52.28 |
| 回到原生 1920×1080 | 50.98 |

缩放仅改变 SCK 输出，源显示模式始终保持 1920×1080 @ 60 Hz。720 输出没有收齐稳定 60 Complete / 秒，BGRA 也未形成明确优势。独立路径仍出现约 51–55 Complete / 秒，说明低帧率不只存在于光梭编码之后；但这不能独立判定瓶颈属于驱动、WindowServer、源合成或其他系统负载。

源时间戳未出现健康性问题不代表已测得端到端延迟。只比较相邻 SCK PTS 的差值，不用 PTS 减本地 `NowUs()`，更不与 Windows 时钟相减；本地回调间隔与源 PTS 间隔应分别保留。Idle 表示系统没有生成新画面，不能直接归类为传输丢帧。[SCFrameStatus](https://developer.apple.com/documentation/screencapturekit/scframestatus)

## 复现命令与权限

先在仓库根目录构建独立探针；构建脚本只构建 / 签名，不自动运行、不设置 TCC：

```bash
mkdir -p build/fps-tests
bash apps/macos-host/probes/probe-build-pattern.sh build/fps-tests/capture-pattern
bash apps/macos-host/probes/probe-build-cadence.sh build/fps-tests/capture-cadence
```

独立捕获须从已获得录屏授权的图形登录上下文启动。默认构建使用独立 probe 标识；如缺少权限，按 macOS 正常方式授权相应诊断进程或启动它的应用，再重新运行。本轮一次直接从 SSH 启动的权限失败不计入性能结果，后续通过现有已授权的 GUI agent 环境完成实测。**不要通过覆盖生产 App 获得录屏权限。**

在一个图形终端运行自有动画源，另一个已授权终端在它持续活动期间运行单项捕获：

```bash
# 终端 A：每组重新启动，确保整个捕获窗口都存在持续动画。
build/fps-tests/capture-pattern --seconds 40 --pacing animation \
  > build/fps-tests/source.json

# 终端 B：独立基线；先确认终端 A 的窗口已出现。
build/fps-tests/capture-cadence --seconds 20 --fps 60 --queue-depth 3 \
  --pixel-format nv12 --max-height 0 > build/fps-tests/cadence-native.json
```

每次只改一个参数并保留独立报告。先做 `60 → 120 → 60`，再做 NV12 / BGRA / NV12；分辨率组使用 `--max-height 0 → 720 → 0`。可选限高值为 **0 / 360 / 720 / 1080**：0 保持原生尺寸，其他值按源比例缩小并取偶数像素，不放大较小的源。报告应核对 `configured_width/height` 和 `image_width/height`，而不是仅看参数名称。

Windows 生产链路 smoke 使用已保存的配对设置，不把令牌写到命令行：

```powershell
New-Item -ItemType Directory -Force build/fps-tests | Out-Null
$smokeReport = Join-Path (Resolve-Path build/fps-tests) 'client-smoke.json'
& dist/windows/RemoteNative.exe --smoke 40 $smokeReport
```

仅运行一个已配对客户端；不要让日常客户端与 smoke 争抢主机连接。源窗口与客户端测试须对齐，再从 JSON 的 `samples` 中选择第 10–30 秒；取值字段为 `at_ms` 和 `fps`。全屏对照只给自有源增加 `--fullscreen`，不修改显示模式；本轮没有将全屏作为生产策略。

每份报告至少核对：`start_succeeded` / `stop_succeeded`、实际 `capture_seconds`、Complete / Idle、无效状态 / 缺图像、PTS 回退 / 重复、实际输出尺寸、回调与 PTS 间隔 P95 / P99，以及源与客户端的有效窗口。256 样本滚动间隔统计不是全程分布，也不能与整段 FPS 直接替换。

## 备份、恢复与后续判断

涉及临时主机调度前，应备份原签名服务、配置与启动方式，记录代码版本和校验结果。独立捕获与生产捕获应分开运行，避免同时捕获增加负载；切换结束后恢复原签名服务并检查连接、硬编码、输入和音频状态。

本轮已恢复原签名服务，bundle 的严格签名校验通过，与本轮备份逐项比较完全一致；不保留 120 捕获间隔、queueDepth 5、BGRA 或全屏设置。完整 Mac 离线回归通过，包括新探针编译 / CLI、唤醒 6 / 6、统计 / 音频 / 日志 / 重启 mock / NV12，以及生产捕获恢复 41 / 41。离线检查不能替代恢复后真实画面、键鼠和声音验收。

恢复后的 25 秒 smoke 退出码为 0，`passed=true`、`hardware_decode=true`；第 10–25 秒最大队列为 0，视频发送拒绝增量为 0，音频丢失、迟到、抖动缓冲欠载、设备欠载与丢弃增量均为 0。这是恢复健康检查，不参与上面的 40 秒对照，也不代表实际按键或可听音质已验收。

下一次先固定负载与源，再重复相同窗口。若源近似 60 Hz 而 metadata-only 捕获仍无法稳定交付，应继续记录捕获侧证据；如果 SCK 已交付约 60 Complete / 秒，而编码接受、接收或呈现更低，再按前轮阶段统计定位下游。记录必须区分请求频率、源近似变化、Complete、队列接受与成功呈现，不能以局部参数调整宣布稳定 60 FPS 已达成。
