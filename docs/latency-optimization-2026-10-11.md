# 光梭低延迟优化记录（2026-10-11）

本轮优先减少光梭自身的等待，并补齐各阶段统计，目标是在持续变化的桌面上尽量稳定交付 60 FPS。修改覆盖 Mac 编码调度和 Windows 视频 worker；目前的结果不能当作稳定 60 FPS 的验收。

Mac 保留 NullMoth **1.9.0** 与 RTX 3060 的 H.264 硬件编码。本轮不修改驱动、不关闭浏览器 GPU 沙盒，也不通过调整浏览器配置绕过初始化问题。图形路径继续使用 Mac ScreenCaptureKit / VideoToolbox 和 Windows Media Foundation / D3D11。此前浏览器与图形 API 的定位见[评估记录](performance-assessment-2026-10-11.md)。

## Mac：用事件唤醒替代编码线程轮询

原编码线程在没有连接、没有待处理帧或编码器仍有 inflight 帧时，每隔约 1 ms 醒来检查状态。现在改用 `EncoderWakeSignal` 的 generation 和条件变量等待。

worker **先记录 generation，再检查连接、编码器和 mailbox**。新帧发布、编码 callback 完成、连接变化、强制 IDR、捕获生命周期变化及退出都会通知 worker；如果通知发生在检查之后、进入等待之前，generation 的变化仍会让等待立即返回。callback 先减少对应 session 的 inflight 数量，再通知 worker。

仍然保留以下约束：

- mailbox 只保留最新帧；编码器忙碌期间新帧替换旧的未消费帧，避免累积延迟。
- 同一 VT session 最多只有一个 inflight 帧，保留实验 NVENC 所需的源缓冲生命周期。
- 每个 session 独立持有 callback 状态；退休 session 的 callback 不会打开新 session 的 inflight gate。
- source 身份和 epoch 继续隔离暂停、停止、重启后的旧回调。编码结果仍需经过捕获生命周期的发表门才能发送。
- 退出显式停止等待；NV12 预打包失败进入既有恢复流程，避免对失败的缓存帧忙等重试。

一次审查发现：旧 epoch 的 recovery block 清除 `_encoderRecoveryScheduled` 后，如果遗漏通知，worker 可能继续睡眠，而新 epoch 的静态帧已在 mailbox 中等待。修复在清除 gate 后发出通知，覆盖了没有下一张动态帧来唤醒的情况。

实现：[encoder-wake.h](../apps/macos-host/encoder-wake.h)、[capture-recovery.h](../apps/macos-host/capture-recovery.h)、[main.mm](../apps/macos-host/main.mm)。

## Windows：记录等待，再修正 worker 定时器

新增滚动统计后发现：原视频 worker 请求 `sleep_for(1 ms)`，但一次实机窗口内实际等待均值约 **15.2 ms**，P95 为 **16.063 ms**。请求的休眠时间不能直接当作实际调度时间。

新版 worker 使用未命名的高精度 waitable timer，设定相对 **1 ms** 的等待，不调用全局 `timeBeginPeriod`。创建、设定或等待失败时，该 worker 关闭 timer 并退回原休眠路径，同时记录失败和回退次数，避免不断尝试失效的 timer。timer 等待采用 **20 ms** 上限；实际恢复调度时间仍受系统负载影响，不能据此承诺退出或调度严格不超过 20 ms。

这仍然是有等待间隔的 worker，尚未把传输 DLL 的新视频到达通知直接接入 Windows worker。修改没有扩大视频队列，没有增加解码线程，也没有切换到 OpenGL / Vulkan。

实现：[worker-wait.h](../apps/windows-client/worker-wait.h)、[session.cpp](../apps/windows-client/session.cpp)。

## 统计口径

双端使用[RollingLatencyStats](../shared/latency-stats.h)：固定保存最近 **256 个样本**，提供累计样本数、窗口样本数、最近值、均值、P95、P99 和窗口最大值。百分位采用 nearest rank；窗口是样本数量窗口，不是固定秒数窗口。

Mac 通过 `host-status` 增加可选字段，保留旧字段和协议。每个阶段包含 `*_samples`、`*_window_samples`、`*_last_ms`、`*_mean_ms`、`*_p95_ms`、`*_p99_ms`、`*_max_ms`。旧主机缺失字段或样本数为 0 时，客户端应显示未观测，不能把它解释成零延迟。

| Mac 阶段 / 计数 | 含义 |
| --- | --- |
| `capture_callbacks` / `capture_complete` / `capture_idle` | 当前有效 source 的屏幕回调、Complete 和 Idle 数量；不是请求的 FPS |
| `capture_interval` / `capture_complete_interval` | 本地时钟测量的全部回调间隔 / Complete 回调间隔 |
| `mailbox_wait` | 新帧从本地入 mailbox 到被编码线程取出的时间 |
| `cached_frame_submissions` | 为静态桌面补 IDR 而重用已消费帧的提交数；不进入 mailbox 等待统计 |
| `prepack` | NV12 兼容路径的目标缓冲分配与 CPU 排布耗时；没有该路径时可能无样本 |
| `vt_submit` | `SubmitH264Frame` 调用耗时，包含可能同步执行的编码及软件 drain |
| `encode_callback` | 提交到编码 callback 入口的本地耗时 |
| `video_send` | 调用 `remote_send` 的本地耗时 |
| `video_send_accepted` / `video_send_rejected` | Rust 传输端本地队列接受 / 拒绝视频的数量；接受不等于已送达 |

原 `captured` 仍保留“准备提交给编码器”的旧口径，不能替代新增的 SCK Complete 计数。

Windows 新增接收间隔、worker 等待、解码输入 / 输出调用、GPU 转换提交、呈现等待 / 调用、待呈现帧等待、处理耗时及成功呈现间隔，并记录 pending 帧替换、呈现忙碌 / 遮挡和 timer 回退。统计界面与 smoke JSON 使用同一快照，便于核对。

**这些统计不是端到端延迟。** Windows 图像阶段记录的是 CPU 墙钟时间，不能当作 GPU 已完成解码 / 转换或显示器已扫描出的时间；成功 `Present` 计数也不是光学刷新测量。接收间隔不是单程网络延迟。Mac 的源 PTS、Mac 本地单调时钟和 Windows 本地单调时钟没有完成同步，不能相减求跨机延迟。

`vt_submit` 与 `encode_callback` 可能重叠，原 `decodeMs` 与细分 Windows 阶段也可能重叠；不能简单相加成“总延迟”。一次窗口中的多个阶段 P95 也不一定属于同一帧。

## 本轮测量与判断

以下是一组受后台负载影响的实机窗口，目的在于定位阶段，不是跨平台性能保证。

| 观测 | 结果 | 解释 |
| --- | --- | --- |
| Mac 图层更新请求 | 约 59.5 次 / 秒 | 应用请求更新的频率，尚非实际可捕获或光学帧率 |
| SCK Complete 回调 | 约 42.5 次 / 秒 | 本次捕获路径交付的新帧频率 |
| 本地视频发送接受 | 约 42.5 帧 / 秒 | 与 Complete 基本一致，编码 / 本地发送没有造成主要帧数缺口 |
| mailbox 等待 | 均值 0.162 ms，P95 0.242 ms | 编码线程取帧等待较小 |
| NV12 prepack | 均值 0.913 ms，P95 1.341 ms | 当前兼容路径的本地耗时 |
| VT submit | 均值 2.688 ms，P95 3.195 ms | 包含同步执行，不能与 callback 阶段相加 |
| 编码 callback | P95 3.093 ms | 此窗口编码耗时不是约 42.5 FPS 的主要瓶颈 |
| 旧 Windows worker 等待 | 均值约 15.2 ms，P95 16.063 ms | 需要修正本地调度等待 |
| 新 Windows worker 等待 | 均值 **1.298 ms**，P95 **1.518 ms**，P99 **1.541 ms** | 60 秒 smoke 在第 50 秒的最近 256 个样本；累计 36,946 次等待，模式为高精度 timer，回退与失败均为 0 |

Mac 同时存在后台编译负载，测试期间的 CPU 调度与图层提交有干扰。图层更新请求频率接近 60，不证明捕获源已稳定生成 60 张不同画面。SCK Complete 到本地发送接受基本没有帧数损失，说明应继续核对源更新、系统捕获和 worker 调度，而不是先扩大队列或重写图形 API。

不同窗口的呈现结果曾约为 **40.8 FPS → 41.9 FPS**。源频率、后台负载和采样窗口未严格保持一致，**不能把这两个数当作改动带来的因果提升**，也不能宣布稳定 60 FPS 已完成。定时器结果只说明这一处本地等待缩短，不能用它推算完整交互延迟减少多少。

最终双端 smoke 持续 60 秒，硬件解码通过，共提交 2,669 帧。剔除启动并选择第 15–50 秒的动画活跃区间后：

- 自有图层测试源共请求约 59.45 次更新 / 秒；Complete 增量约 42.60 / 秒，本地视频发送接受约 42.63 / 秒、拒绝为 0。客户端每秒采样的呈现率中位数约 43.94 FPS，范围约 36.99–49.94；这些计数的窗口与更新时点并不完全相同。
- 待处理视频队列始终为 0；pending 帧替换增量为 0、呈现忙碌与遮挡增量为 0。观察到 3 次短暂的呈现就绪等待超时，仍保留待呈现帧供后续尝试，没有当作连接失败或稳定 60 FPS 的证明。
- 第 50 秒 mailbox 等待 P95 为 0.234 ms、编码 callback P95 为 3.189 ms，Windows 帧处理 P95 为 0.869 ms。仍需分别理解这些有重叠的阶段。
- 音频保持 48 kHz 双声道、低延迟播放，35 秒收到 7,000 包（200 包 / 秒）；区间内丢包、晚包、丢弃、jitter 欠载、设备缓冲欠载与设备重启计数均无新增。整个会话启动阶段有 1 次丢包及对应欠载，设备缓冲欠载累计为 0，不能写成全程零丢包。

smoke 新增 `audio_timing` 元数据，与视频阶段一起每 500 ms 采样。它不保存 PCM 内容；本轮验证了调度与缓冲计数，尚未替代真实声音的主观听感验收。

## 回归与故障对照

- Mac 编码唤醒测试在 Mac 和 Windows 本机运行，各 **6 / 6** 通过：覆盖通知早于等待、无任务阻塞、唤醒、停止及 2,000 轮发布 / callback 竞态，保持 latest-only 和 single-inflight。
- 生产捕获恢复 probe 使用自有合成 `CMSampleBuffer` 调用真实生命周期方法，不启动桌面捕获、不读取配对令牌，也不操作系统 UI。修复版 **41 / 41** 通过，0 项跳过。
- 对照版只移除旧 recovery 清 gate 后的通知，得到 **40 / 41**：唯一失败是 `static_pending_idr_woken_after_stale_recovery`。恢复通知后该项通过，证明测试覆盖了所修复的丢唤醒窗口。
- Windows worker timer probe 覆盖创建 / 设定 / 等待失败、超时后的回退、停止竞争及 handle 清理，并单独记录真实 timer 采样；不以脆弱的固定耗时阈值替代正确性测试。

测试源：[encoder-wake-test.cpp](../apps/macos-host/probes/encoder-wake-test.cpp)、[capture-recovery-probe.mm](../apps/macos-host/probes/capture-recovery-probe.mm)、[worker-wait.cpp](../apps/windows-client/probes/worker-wait.cpp)。本机完整 Windows 与 Mac 离线回归、双端构建、双语界面检查及公开源码扫描均通过。实机使用 `0.3.0-dev.latency` 开发构建，未创建发布版本或 tag。

## 以后继续优化的复现顺序

1. 先保存双端正在运行的可执行文件、配置和签名信息；驱动 / EFI 如不在本轮范围内就保持原版本。不要把令牌、机器标识、Apple 账户信息或个人日志提交到公开仓库。
2. 记录代码版本、构建架构、实际编码器模式、显示器分辨率 / 刷新率、后台 CPU 负载和 timer 模式。设置请求的 60 FPS 不可替代实际源计数。
3. 构建 transport 和双端，再运行离线回归。Mac 入口为 `bash scripts/test-macos-ci.sh build/ci-tests/macos path/to/libremote_transport.a`，已包含生产捕获恢复 probe；核对 41 项结果。Windows 使用 `scripts/test-windows-ci.ps1 -RuntimeDirectory path/to/runtime -ToolchainDirectory path/to/compiler/bin`。
4. 使用同一种持续变化的自有测试窗口，先测更新请求间隔和 SCK Complete，再测发送接受、接收、解码及成功呈现。记录样本数与滚动窗口，剔除启动、窗口关闭和静态尾段。
5. 固定源、分辨率、编码模式和码率，一次只改变一个调度 / 队列参数。空闲与后台负载场景分别测量，重复相同窗口，不能直接比较不同源频率的总 FPS。
6. 验证动态画面之后，再检查静态桌面强制 IDR、暂停 / 恢复、断线 / 重连、编码器退休 / 替换与退出。低延迟改动不能以卡住静态桌面或泄漏缓冲为代价。
7. 确认至少存在稳定的 60 Hz 捕获源后，再开展码率或更高刷新率对照。码率提高不应靠扩大待处理队列补偿；每次测量保留 P95 / P99 和音频状态。

可单独重跑 Mac 事件唤醒测试：

```bash
mkdir -p build/latency-tests
xcrun clang++ -std=c++20 -O2 -Wall -Wextra -Werror \
  apps/macos-host/probes/encoder-wake-test.cpp -o build/latency-tests/encoder-wake
build/latency-tests/encoder-wake
```

后续记录应写清楚“改了什么、实际观测到什么、还有什么未验证”，保留阶段数据和失败对照；避免把请求 FPS、API 返回成功或局部毫秒统计包装成已经完成的端到端目标。
