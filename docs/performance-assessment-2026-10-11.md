# 光梭低延迟与图形 API 评估（2026-10-11）

目标是低延迟优先，在持续变化的 60 Hz 桌面上尽量稳定交付 60 FPS。以下结论属于本次 Windows x64 / Intel Mac VM 的实测，不能外推到其他 GPU、驱动或 ARM64 设备。

随后完成的编码事件唤醒、Windows 高精度等待和分阶段计时见[优化记录](latency-optimization-2026-10-11.md)。

## 当前决定

保留 Mac 的 ScreenCaptureKit / VideoToolbox 和 Windows 的 Media Foundation / D3D11 路径。OpenGL / Vulkan 的基础能力已测，但没有证据表明改写为这些 API 会降低当前链路的延迟。

用户决定暂不深入研究或修改实验驱动。Mac 保留 NullMoth **1.9.0** 与 RTX 3060 的 H.264 NVENC 硬编码；本轮没有安装驱动补丁、升级驱动、关闭浏览器 GPU 沙盒或修改用户浏览器配置。

## B 站约 8 FPS 的定位

Chrome **155.0.8059.40** 的 GPU 报告确认：Compositing、Canvas、Rasterization、Video Decode 全部软件运行，OpenGL / WebGL 禁用。报告显示 GPU 进程反复初始化失败；错误包含 `Initialization of all (1) EGL display types failed` 和 `GLDisplayEGL::Initialize failed`。浏览器原本请求开启硬件加速，这不是仅凭设置开关就能排除的问题。

两个全新临时 Chrome profile 的隔离测试保留了 GPU 沙盒，不操作用户页面或登录资料。ANGLE Metal 后端同样无法建立 WebGL context；ANGLE GL 测试也失败，日志明确指出此 Chrome 构建允许的实现只有 ANGLE Metal。这些测试仅验证初始化，不能替代实际网页播放测试。

| 场景 | 观察 | 能支持的结论 |
| --- | --- | --- |
| Chrome 播放 B 站，18 秒独立协议探针 | 约 17.1 秒计数窗口内，主机提交与编码均约 **5.62 帧/秒**，编码失败和 mailbox 丢帧增量为 0；接收端每秒收到约 3–8 个视频记录；仍使用 NVENC | Windows 解码器之前就只有少量视频帧；不是客户端把完整 60 FPS 解码成了 8 FPS |
| 同一主机播放自有动画，实际 Windows 解码与呈现 | 动画源 18.05 秒绘制 **1010 次，约 55.95 次/秒**；活跃区间客户端多次采样约 **55.7–57.5 FPS**，一次约 51.8；待呈现队列为 0 | 光梭整条链路可以超过 8 FPS；尚未证明持续稳定 60 FPS，因为动画源本身约 56 FPS |
| Safari 打开同一页面 | 用户观察约 **30 FPS** | 浏览器路径有明显差异；尚未独立核实视频原始帧率、实际解码模式与长时间稳定性 |

主机 `captured` 计数实际发生在编码提交附近，不是 ScreenCaptureKit 原始回调计数；`encoded` 计数也不代表网络已成功交付。此次协议探针不做 Windows 解码，动画测试则走实际 Media Foundation / D3D11 路径。必须分别理解这两类证据。

Windows 面板的 FPS 是成功的 `Present` 调用率，不是物理屏幕扫描次数。静态桌面和 24 / 25 / 30 FPS 视频通常不会持续提供 60 个不同的画面；不应靠重复帧来让计数变成 60。ScreenCaptureKit 的 [minimumFrameInterval](https://developer.apple.com/documentation/ScreenCaptureKit/SCStreamConfiguration/minimumFrameInterval) 限制最高更新频率，不保证源端每秒生成相同数量的完整帧。

上游 [v1.13.0 的 NVK 补丁](https://github.com/nullmoth/nvidia-macos-driver/blob/v1.13.0/nvk/nvk-macos.patch) 将用于占位文件描述符的 `/dev/null` 打开方式由读写改为只读，并说明浏览器 GPU 沙盒的权限问题。本机 1.9.0 二进制仍使用旧方式。它是与现象吻合的线索，**没有通过修补后的对照测试证明它是唯一原因**；按用户决定停止驱动深层研究。本次也没有把 1.13.0 的库混装进 1.9.0。

测试期间 Mac 另有占用多个 CPU 核心的编译任务，这是性能结果的干扰项。没有停止该任务，也不能把上述短测当作空闲机器的极限成绩。完整浏览器 GPU 报告、连接凭据和原始私有诊断文件未发布。

## 双端 OpenGL / Vulkan 实测

| 端点 / API | 通过的基础测试 | 实际边界 |
| --- | --- | --- |
| Mac OpenGL | 加速 CGL context；Core profile 报告 **OpenGL 4.1 / GLSL 4.10**；FBO 清屏像素、GLSL 编译链接与三角形像素校验通过 | GL-on-Metal 仍经过实验驱动；不是原生 NVIDIA OpenGL 4.6，也没有完成应用兼容性或性能验收 |
| Mac Vulkan / NVK | loader、instance、device 创建；设备报告 **Vulkan 1.4.363**；GPU buffer copy / fence / 512 个整数校验通过 | 安装的 ICD 声明 1.3.0；未提供 `VK_KHR_surface`、`VK_EXT_metal_surface`、`VK_KHR_swapchain`。本轮只证明离屏命令可用，没有验证可呈现的 Vulkan 桌面链路或完整一致性 |
| Windows OpenGL | Radeon RX Vega 报告 **OpenGL 4.6**；硬件像素格式与清屏像素校验通过 | 宣告 WGL / D3D 互操作扩展；尚未实际导入 Media Foundation 的解码纹理 |
| Windows Vulkan | **Vulkan 1.3.217**；Win32 surface / swapchain 扩展；GPU buffer copy / fence 校验通过 | BGRA8 / NV12 的 D3D11 texture 外部内存属性报告可导入、需专用分配、不可导出；属性查询不等于真实解码纹理互操作成功 |

当前 Windows 已通过同一个 D3D11 device 做硬件解码，使用 GPU 的视频处理器完成 NV12 转换、缩放与呈现，没有 CPU 视频像素回读。改成 OpenGL / Vulkan 会增加纹理共享、同步和显示路径的实现与验证工作。Mac 改用 OpenGL 也不会消除当前 NVENC 兼容路径中的 CPU NV12 重新排布。

这些基础探针均正常退出，没有安装图形 SDK 或改变系统驱动。API 版本号、扩展枚举和简单像素通过不能当作完整图形栈的验收。

## 带宽与刷新率

物理链路为 1 Gbps Ethernet。一次约 3 秒、内存到内存的 Mac → Windows TCP 测试，接收速率约 **940 Mbps**。它证明当时的基础链路容量，不等于 QUIC 视频吞吐、拥塞时延或长期稳定性。

当前硬件编码请求 **25 Mbps / 60 FPS**；软件回退请求 **8 Mbps / 30 FPS**。VideoToolbox 的 [ExpectedFrameRate](https://developer.apple.com/documentation/videotoolbox/kvtcompressionpropertykey_expectedframerate) 是编码提示，码率属性请求成功也不保证编码器严格执行。B 站低帧率探针中的编码流量约 0.12 Mbps，暂时没有证据指向平均带宽耗尽。

提高码率可能改善高运动画面的压缩质量，不能修复 Chrome GPU 初始化失败，也不能让低帧率视频生成更多原始画面。当前是 8-bit NV12 / H.264 4:2:0；仅加码率不会获得 4:4:4 文本色彩或 HDR。Mac 显示源为 1080p / 60 Hz，评估 90 / 120 FPS 前要先建立真实对应刷新率的显示源。

## 光梭后续优化顺序

1. **先补齐阶段统计。** 分别记录 ScreenCaptureKit 的 complete / idle 回调、源时间间隔、NV12 排布耗时、编码提交 / 回调耗时、传输接收与成功呈现计数。展示平均值与 p95 / p99，避免只凭一个 FPS 数字判断瓶颈；现有解码耗时包含 CPU 提交和呈现等待，不能称为 GPU 解码耗时或端到端延迟。
2. **用已知帧率的持续动画做基准。** 在没有额外重负载的条件下，以实际 60 Hz 更新源做长时间双端测试，再测试浏览器、滚动、视频、文件传输和系统音频并发。分别报告缺帧、长间隙、队列、画面完整性与音频欠载；当前短测不满足稳定 60 FPS 的完整验收。
3. **在保持低延迟队列的前提下比较码率。** 先保持现有布局正确性与单帧编码在途限制，逐档比较 25 / 35 / 50 Mbps 的质量、编码用时、网络突发和呈现间隔。这是待实施的实验设计，尚未提高生产码率。不要用增加排队深度掩盖卡顿。
4. **按测量结果优化。** 优先处理实际耗时高的 NV12 排布、等待或 GPU 转换，评估用唤醒替代轮询；正确性和延迟一起验收。只有互操作原型和 GPU 耗时证明收益后，再考虑替换 Windows 的呈现 API。

暂时推荐使用 Safari 播放相同内容，继续保留 H.264 硬编码远控。以后评估新驱动时，浏览器加速、真实视频播放和光梭 NVENC 像素正确性必须同时重新验证，不能只看 Metal 或 API 初始化成功。
