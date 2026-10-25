# 第三轮审计：修复、验证与剩余门禁

审查基线为 `a0da0a2ba693d3484dcaa1e7727f38a70c87d12f`。本轮逐项核对第三份报告的 **13 项发现**，并处理了另外两项实证发现：COM 枚举错误和软件 loopback 候选误选。多条失败断言、多个轴或多个测试场景不重复计为缺陷。

候选版本为 Android **1.5.17-round3 / code68**、Host **1.1.6**。这是有明确剩余数值门禁的测试迭代，不能表述为“所有测试通过”或“实战跟随已经达到要求”。最终构建和交付身份见文末。

## 保持的合同

原 Overwatch W8A16 图、原生 **640×640** 输入/采集/控制坐标、身体框顶部向下 **18%** 的几何瞄点、原校准作用域、MAKCU 与蓝牙输出路线保持。没有增加独立头部模型，也没有用模型替换掩盖链路问题。

保留用户已选择的快切例外：**唯一、高置信、真正新陌生目标，两次独立新鲜观察且至少 8 ms**。弱置信、多人歧义、已知邻人及 tracker 暂留的形状不借用这个例外；普通确认合同仍独立存在。

本轮未修改用户真实网卡、防火墙、Clash、DNS 或路由，也没有为了验证而停止正在使用的 Host、切游戏前台、发送真实鼠标动作或触发 UAC。源码中修复未来自动配置的安全约束，与在当前用户网络上实际执行改配是不同的事情。

## 原报告 13 项逐项核对

| 编号 | 确认的问题 | 已实施措施 | 验证范围与限制 |
| --- | --- | --- | --- |
| **H3-02 / P1** | persistent 偏好可绕过已有兼容手工 IPv4 的非破坏性要求，旧快照又不足以重建它。 | 地址保留约束先于后端偏好；受保护地址只能使用安全 alias，失败后禁止 persistent 地址模式 fallback。静态 DNS、非兼容静态地址及普通有网关 LAN 在修改前拒绝。snapshot v3 增加原手工地址的只读 guard，恢复不能只凭 DHCP=true 宣称成功；普通空白 DHCP 下游保留 persistent 优先。 | 原源码复现错误选择；生产 candidate→plan→backend record→mutation→verify/rollback 组合正反及故障注入通过。v3 序列化/解析、v1/v2 读取兼容和缺 alias 负例通过。没有执行真实网卡改配或提权 worker。 |
| **R3-C3 / P2** | 同模型/路线重配仍只搬运旧 7 项校准，已知延迟范围被缩成 nominal 单点，低估未显现风险。 | 同 scope 使用有类型的运行时校准，保留比例、quality、nominal、可行范围、资格、restored reference 与小型 nominal-history 先验；不迁移旧命令/观察。失效或不匹配的缓存不能恢复更早单点值。 | 实际 Bridge/Core 重配置、空/旧缓存、重复应用、scope 切换、损坏范围和新可辨识证据回归；不是只比较数组。原 24–100 ms 风险范围得以保持，90 ms 时的新命令仍受风险预算约束。物理输出未验。 |
| **R3-C2 / P2** | 全局 pending 阻止独立静止轴进入量化终态，另一轴持续运动会保留原往返。 | 在所有可行延迟下分别统计非零 X/Y 报告；不以带符号净位移为零当作没有风险。Core/Servo 按轴确认、进入和维持半计数终态。 | 真实 Core 双轴闭环 24 场景、完全抵消但未显现的相反命令、延迟修订、另一轴随后启动，以及原 432 静止组合。不能把风险计数当作 USB 队列或执行回执。 |
| **R3-C1 / P2** | 两条速度历史都把合法稳定 30/20 FPS 清空，比例校准可学习但实际前馈仍为零。 | world 与 retinal/disturbance 链统一使用真实相邻 source 时间；明确连续证据允许最多 50 ms。按物理时间窗、最低独立点数和原 150 ms 上界管理稀疏历史；真实缺帧/陈旧/倒退不能积累运动债务。明确反向证据可退役旧拟合为 unknown，不用单点制造新反向速度。 | 72 个正向周期/抖动/轴/链路场景及间断、背景无效、未知比例、手动跟随、形状/噪声/反转、HID 歧义负例。**根因专项回归通过，但最终综合动态场景有新增数值退化，见剩余门禁；不能标成全部性能验收通过。** |
| **R3-J01 / P2** | 首次消费的旧报告仍可能覆盖其后已完成的同步按钮授权。 | coordinator 拥有独立全局 authorization revision；采样前捕获，样本携带，最终比较与 reconcile 在同一 monitor 中完成。过时授权字段丢弃并重采样，真实撤权/故障检查保留。 | 真实 coordinator 固定交错覆盖首次消费、当前 revision 撤权、松键、退役路线、已经关闭时的显式关闭及健康恢复。未把阻塞 native 采样搬回 UI。 |
| **R3-J05 / P2** | 旧 codec 工作线程的无归属 fatal JNI 可将新 decoder 置失败。 | 每个 codec 捕获不可变 binding/stream/codec token；fatal 与 format mutation 在 native 同一锁内验归属。旧 restart job 不能借新 stream 身份发布 codec，迟到完成也不清掉新 codec 的真实 fatal。 | 原方法体故障复现、实际 ownership helper、完整生产 NDK 翻译单元和 Java 编译。最终 APK 必须重建 native，不能 Java-only 复用旧库；真实 MediaCodec 调度和运行时 JNI 仍属设备验证。 |
| **R3-J04 / P2** | 未验证 probe 就发布 requester，首次无线发现可能锁定无效发送者。 | Ready 接收完成格式、token、padding、source、代次和新鲜度核验后才发布 peer。首次从未工作路线只可在 10 秒宽限后、出现不同新鲜候选时尝试重发现；native 原子 claim 与首个合法视频包接纳共锁，最终复核 network/local/host scope。 | 完整 receive→validate→publish→ACK 类、epoch/time 负例、首包/claim 次序和 Java 路线策略。已工作路线、CAT6、同 host、旧 scope 不会因旧零计数被重置。不是主机账号认证，也不是完整 Service 实机恢复验收。 |
| **R3-J02 / P2** | 16 字节按钮接收缓冲把超长报文的合法前缀当作完整快照。 | 接收容量为 17，且每次重置；只接受精确 16 字节，再验证来源、格式、session、sequence，成功后才提交状态。 | 实际 JVM loopback socket→生产 decoder，含 0/15/17/4096 字节、坏 mask/reserved/session、错来源和重放；后续合法包仍可接受。没有使用活动应用端口或当前游戏网络。 |
| **R3-J03 / P2** | Ready 将零长 UDP 与系统调用失败混用，可能读取旧 errno 并退出。 | 正/零/负接收分开；只有负返回使用此次立即捕获的 errno；空包/无效包不终止正常 worker，不提交 peer。 | 生产类与实际 ARM socket 边界覆盖空/坏/超长/有效包和真实 closed-FD 故障。`probes_received` 保留所有正长度 datagram 的原语义，不能冒充合法 probe 数；ACK、invalid、empty 分开记录。 |
| **R3-J06 / P2** | 本地创建/打开文件失败的 FileNotFoundException 被误判为永久包不兼容。 | 在打包资源源头建立有类型的错误来源；本地文件 I/O 可按原退避重试，明确缺资源、空资产、完整性不匹配仍保留终态。 | 两个真实 installer 的临时文件故障、退避及移除障碍后成功重装；缺包和损坏数据负例。小型合成资产不会加载 QNN，也不代表设备文件系统权限已实测。 |
| **H3-01 / P2** | 同 EXE 合法 legacy 防火墙规则在前置第二层只按新名称查找，迁移不可达。 | 当前与明确登记 legacy 名统一解析到 desired policy；之后继续完整 ownership 核验。生产 COM 与隔离 store 共用 audit、collect、reconcile、验证和 rollback。 | 实际生产 COM 适配器配内存 INetFwRules、本地规则对象，覆盖 legacy/current/duplicates、字段伪造及添加/删除失败的完整 multiset 回滚。没有创建系统 Policy 或写真实防火墙。 |
| **R3-R01 / P2** | 三个诊断入口仍用 416/208，误判 640 坐标、框可见性及渲染画布。 | 使用逐行坐标维度和实际视频大小；框/瞄点按各自坐标域缩放，区分帧采集与之后提议的时间含义。归档要求 shape 元数据或明确声明宽高/legacy 模式，非法/冲突维度拒绝。新采集声明实际 tensor 尺寸。 | 实际 CLI 覆盖 416/640/1024/矩形、held/tracking 排除、原生/缩放视频、边缘/内部框及旧归档。诊断不能替代人工标注或物理执行。 |
| **R3-R02 / P2** | 动态拟合跨日志缺行积分，背景位移与命令/时间覆盖不一致。 | 按 process/trace/generation 与连续 control-id/time/background 切段；重复或倒退拒绝；bin 和延迟命令覆盖不得跨洞。合法 wire-frame 跳号不等于 control 观察缺行；覆盖不足返回 no-fit。 | 实际 CLI 连续对照、dt 仍小于 20 ms 的删行、稠密/多洞、重复、倒退及同 clock/id 的多 trace/generation。保留显式拟合帧期合同；相关拟合不是物理 HID 时延测量。 |

## 两项额外发现，单独计数

### COM 枚举错误：失败被当作空集合

生产的两处 COM 枚举先判断 fetched=0，再判断 HRESULT 失败。隔离真实 COM 适配器收到 E_FAIL/零项时，错误地继续进行规则迁移；fixture 从 5 条变成 10 条，却报告 rollback complete。

现在先判失败，只接受合法结束和计数。相同故障在 audit 阶段拒绝，规则保持 5 条且零写入。该发现来自完整组合故障注入，与 H3-01 的 legacy 名称错误是两个根因；不把多个重试/场景重复计数。

### H3-03：软件 loopback 的硬件标志误报

本机只读核对证实，曾被选为下游的软件 loopback 同时报告 HardwareInterface=true、Virtual=false、ConnectorPresent=true。其 PnP 硬件 ID、service 和 INF 则明确对应已知 Microsoft loopback。

生产枚举现按 adapter GUID 关联只读设备身份，精确排除已确认的 loopback，不按 FriendlyName 猜测，也不因 PhysicalMediumUnspecified 一刀切拒绝真实 USB/PCI。身份未解析不等于虚拟，但在 snapshot、提权和改配前拒绝自动 mutation，保留只读已有地址探测及原 LAN 回退。此类旧恢复记录被保留并安全拒绝，不顺手删除现有地址。

MIB/PnP→chooser→plan/mutation 组合、PCI/USB 正例、未知身份零写入和实际只读枚举均验证。没有为验证修改当前接口、启动新 Host 或运行真实改配。

## 已执行证据与最终统计分开

- Java 组内 **56 组行为合同**通过，包括真实临时文件、loopback UDP、授权 revision 和 callback token 的正反例。
- 诊断 **8 组实际 CLI**最终通过；另有原 12 组脚本回归、5 个既有诊断入口、native-dependency 5 组及 profile migration 5 组通过。采集器还在任何 ADB 调用或元数据写入前拒绝包含 `bursts.json` 或 `detections-*.csv` 的已有目录，保护上次中断后尚未形成汇总的原始证据。
- Host 专项 **6 个 CTest targets**通过，含真实 COM 适配隔离 store、地址事务、snapshot 与 PnP admission；这些操作没有写真实网络。
- Ready ARM loopback 二进制此前已在手机执行通过，使用临时 kernel-local socket，不使用活动 App 端口、NPU、USB/HID 或 UI。最后收尾重新编译出的二进制与已执行版本逐字节 SHA 相同、生产处理类 hash 相同；因 ADB 未在线，**没有把重编译说成再次实机执行**。这仍不是完整 Android Service / 真实 LAN 恢复验收。
- 所有根因专项 fixture、构建、完整 integration suite 和真实物理效果分别记录；以上局部通过不推导出全 suite 通过。

最终 MSVC 完整清编完成 361 个生成步骤；活动依赖清单验证 263 个编译对象，`passed=true, issues=[]`。CMake 注册 93 项，其中 4 项明确排除：`dxgi_capture`、`nvenc_runtime_probe`、`nvenc_session_probe`、`desktop_video_agent`，避免操作真实桌面/GPU。其余 **89 项：86 通过、2 跳过、1 失败**，26.53 秒。跳过项为 Windows 不适用的 ARM 像素 parity 和 POSIX Ready loopback；失败项是 `vfdual_mobile_observation_servo_tests`，原断言保留。对应本地证据为 `full-native-clean.log`、`full-native-junit.xml`。较早边改头文件边增量编译时出现的 trace/round3 异常，在新独立一致 ABI 树及最终 clean 全树均未复现，不将其写成已确认的生产内存损坏。

## 控制性能：保留未达标及新增退化

冻结控制专项的逐函数对照为：不可变 a0 基线 **27/29**，当前 **26/29**。同样为红的历史目标是 `test_production_severe_dropout_retains_shipping_baseline` 与 `test_detector_jitter_does_not_become_camera_oscillation`；本轮还新增普通低 FPS 综合场景未达标。因此不能宣称严格逐指标非回归。

| 冻结控制专项 | 当前结果 | 原门禁/含义 |
| --- | --- | --- |
| 普通 30 FPS / scene1063 | RMS **40.665**、P95 **75.646**、0 次逻辑换锁 | 原 RMS<40 未达到；该项是本轮新增退化，不能混为基线已知失败。 |
| 历史 severe 60 FPS | RMS **33.983075**、P95 **66.329578**、2 次逻辑换锁 | 保持原门禁，不因本轮修复扩大阈值。 |
| 历史 severe 30 FPS | RMS **49.291020**、P95 **107.297076**、6 次逻辑换锁 | 严格历史目标仍未达到。 |

这些是指定合成轨迹中的控制结果。历史 severe 使用历史压力配置，不将其重定义为当前原生 640 生产视野，也不将纯框的逻辑身份当成物理人物真值。本轮没有继续扫描 gain/pacing 或修改旧阈值制造“全绿”。

最终 `control/final-freeze.json` 中的全部控制源 hash 已与交付工作树逐项核对；APK 构建前后源 hash 也保持一致。原生完整 clean 测试中的 trace、round3、量化、校准专项均通过；默认 Servo 目标仍失败。其默认 main 会在先遇到的 severe 断言结束，不能因此把后面的噪声和普通低 FPS 函数记成通过；上表及 26/29 来自相同生产 TU 的逐函数独立对照，完整输出保存在 `control/servo-coverage/`。没有使用中间版本的 39.790 / 27-of-29 作为最终成绩。

## 构建与交付状态

新增 JNI 与原生接口要求完整 native 重建，不能通过旧 APK 的 native-baseline 包装替代验证。最终必须分别核验 APK 内容/签名、原模型 hash、Host 版本和成对产物 SHA。

| 项目 | 最终交付记录 |
| --- | --- |
| Android 版本与 APK 路径 / SHA / native 库身份 | `1.5.17-round3` / code68；`dist/candidates/round3-20261025/VisionForge-Local-1.5.17-round3.apk`；SHA `b570ceb45920eccbcd59e322d864fc3fb62c875bbb883e9b5fe6c2d448d22629`；runtime so SHA `1ebd8ca2d2c61a72b41c80604c3d5cef4c8e662038aa85a54f81135b59f685bd`。15 个 native 对象强制重编并 fresh link，签名和六个新 JNI 导出核验通过。 |
| Host 版本与 EXE 路径 / SHA | `1.1.6.0`；`dist/candidates/round3-20261025/VFHost-Local-1.1.6.exe`；最终 clean 构建 SHA `5e2e5f014c9d82f4ddbf50c7f586a36def786575cce3034ee9323586ca16bae8`。 |
| 源码提交 | Host `d5db9a3`；诊断 `3a8ea0c`；完整控制/Android/JNI/CMake `b4ac9c1c652a6f94939eee7540dbf140030a13b5`；中断采集证据保护 `8b14828`。最终合并身份以仓库提交历史及交付记录为准。 |
| 安装与运行状态、是否仅交付候选 | **未安装新 APK、未启动或替换新 Host。** 最后 ADB 设备及 mDNS 列表为空；既有固定路径 Host 1.1.5 保持运行且 SHA 未变。候选 `formal_stability_gate_passed=false`，未作真实游戏/物理控制验收。 |

同一路径的 EXE 会因最终 RC/link 重建改变 hash，必须使用最终文件身份，不能引用较早局部构建的中间 hash。旧安装包、候选、失败日志及原有校准保留；原始本地 `audit/round3-20261025/` 证据不随公共仓库上传。

原图 SHA 保持 `7b650a9b885a28863370c9b988a8e16f63cf2a8c2a6775de087ca1c47554a8c4`；安装包没有加入其它游戏或头部模型。候选目录包含 `SHA256SUMS.txt`、`validation.json` 和交付说明。Host 规则绑定原固定 EXE 路径，不能通过从临时候选目录直接启动去验证原防火墙规则；本轮没有做该切换。

真实鼠标输出、手机完整长测、MediaCodec 调度、实际游戏对局与命中效果尚未由本轮离线回归验收。报告不承诺零时延、零抖动或所有复杂场景稳定锁定。

## 复跑入口

在已配置 MSVC/Android SDK/NDK、JDK 与 Python 依赖的本地环境中，可使用仓库保留的测试入口：

```powershell
python scripts/test-round3-diagnostics.py
python scripts/test-report-regressions.py
python scripts/test-native-dependencies.py
python scripts/test-control-profile-migration.py
python scripts/test-java.py
scripts/build-android.bat --offline
```

本次 MSVC 实际构建使用 Ninja / Release / `CMAKE_OBJECT_PATH_MAX=240`，在 MSVC developer shell 运行以下序列；`--clean-first` 只清理该构建目录的生成产物：

```powershell
cmake -S dual_machine_runtime -B build/h3-host -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OBJECT_PATH_MAX=240 -DVFDUAL_ENABLE_PRIVATE_HOST_SYMBOLS=OFF
python scripts/repair-native-dependencies.py --build-dir build/h3-host
cmake --build build/h3-host --clean-first --parallel 2
python scripts/repair-native-dependencies.py --build-dir build/h3-host --verify
ctest --test-dir build/h3-host --output-on-failure -E "vfdual_(dxgi_capture|nvenc_runtime_probe|nvenc_session_probe|desktop_video_agent)_tests"
```

Android 最终 fresh native target 完成后，在 `android_inference_benchmark` 目录运行 `gradlew.bat :app:assembleRelease :app:assembleReleaseAndroidTest -PheadlessBenchmark --offline --max-workers=1 --console=plain`。项目实际 testBuildType 为 release；构建 AndroidTest 不代表已运行 instrumentation。

原生测试由 CMake 清单注册；Windows 跳过的 ARM 专用目标与明确排除的桌面/GPU 目标必须分开报告。涉及 ADB、真实系统规则/网卡或物理输入的现场测试应另行记录，不能用以上脚本的通过状态代替。
