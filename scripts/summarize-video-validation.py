"""Summarize completed device runs, with explicit sampling and measurement scope."""
from pathlib import Path
import csv, json, re, datetime

root=Path(__file__).resolve().parents[1]
audit=root/'audit/control-20261018'
directory=audit/'video-foreground-final2/video-headless'
report=json.loads((directory/'video-replay.json').read_text(encoding='utf-8'))
assert report['passed'], 'Only a completed successful run may be summarized'
phase=report['phases'][0]
assert phase['expected_frames']==10160 and phase['received']==10160

def timings(value):
    result={}
    for name in ['decode_queue','preprocess','qnn','receive_to_result']:
        match=re.search(r'\b'+name+r'\{([^}]+)\}',value)
        assert match, name
        result[name]={k:int(v) for k,v in re.findall(r'(n|p50_us|p95_us|p99_us)=(\d+)',match.group(1))}
    return result

last=timings(phase['decoder'])
samples=phase['samples']
thermal=sorted(set(s['thermal_status'] for s in samples))
failures={name:int(re.search(r'\b'+name+r'=(\d+)',phase['decoder']).group(1))
          for name in ['qnn_failures','preprocess_failures','decoder_failed','stream_restart_failures',
                       'dropped_access_units','stale_decoded_outputs','superseded_decoded_outputs']}
assert not any(failures[name] for name in ['qnn_failures','preprocess_failures','decoder_failed','stream_restart_failures']),failures
trace=list(csv.DictReader((directory/'ow_m_416-detections.csv').open()))
frames=set(r['frame_id'] for r in trace)
positive=set(r['frame_id'] for r in trace if float(r['confidence'])>=.2)
high=set(r['frame_id'] for r in trace if float(r['confidence'])>=.4)
local_report=json.loads((audit/'video-local-final/video-headless/video-replay.json').read_text(encoding='utf-8'))
assert local_report['passed']
local_phase=local_report['phases'][0]
summary={
    'device':report['device'],'soc':report['soc'],'process':report['process'],'scope':report['scope'],
    'passed':True,'frames':phase['executions'],'media_seconds':10160/30,
    'run_wall_seconds':(report['finished_unix_ms']-report['started_unix_ms'])/1000,
    'thermal_statuses':thermal,'failure_counters':failures,
    'final_rolling_300_frame_window':last,
    'qnn_window_p50_range_us':[min(timings(s['decoder'])['qnn']['p50_us'] for s in samples),
                               max(timings(s['decoder'])['qnn']['p50_us'] for s in samples)],
    'first_replay_detection_frames':{'total':len(frames),'confidence_ge_0_2':len(positive),'confidence_ge_0_4':len(high)},
    'device_local_soak':{'passed':local_report['passed'],'received':local_phase['received'],
                         'executions':local_phase['executions'],'scope':local_report['scope']},
    'background_wireless_soak':{'passed':False,'sent':10160,'received':9608,'executions':9448},
}
(audit/'validation-summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2),encoding='utf-8')
table='\n'.join('| '+label+' | '+ ' | '.join(f"{last[name][key]/1000:.3f}" for key in ['p50_us','p95_us','p99_us'])+' |'
    for name,label in [('decode_queue','解码等待'),('preprocess','预处理'),('qnn','NPU 推理'),('receive_to_result','手机接收到结果')])
started=datetime.datetime.fromtimestamp(report['started_unix_ms']/1000,datetime.timezone(datetime.timedelta(hours=8))).isoformat()
text=f'''# VF 本地单模型版验证记录 — 2026-10-18

## 交付与测试结论

当前版本 1.2.0-overwatch，只有 Overwatch YOLO11m 416 / QNN W8A16。手机实际安装文件与交付 APK 的 SHA-256 一致。原项目 863 个文件重新核对无变化；卡密依赖、安装包模型数量和交付哈希另见 `../verification.json`。

本轮前台无线复测从 {started} 开始。独立进程 `{report['process']}` 使用生产 UDP 接收、硬件解码、预处理和 QNN NPU，完成 8 次循环，接收 10,160 / 10,160 帧、执行推理 {phase['executions']} 帧。VF Activity 保持可见；其常规服务的接收管线暂停，防止与测试管线争用。媒体时长 {summary['media_seconds']:.2f} 秒，包含准备和收尾的测试墙钟时长 {summary['run_wall_seconds']:.2f} 秒。QNN、预处理与解码错误为零，测试正常结束。

错误与旧帧处理计数：`{json.dumps(failures)}`。stale / superseded 是主动丢旧帧计数，不是 QNN 执行错误。Android 热状态采样集合：`{thermal}`（0 表示 NONE）。这只是约六分钟的验证，不等同于数小时的温控测试。

手机本地回放另完成 8 次循环：接收 {local_phase['received']} 帧、执行推理 {local_phase['executions']} 帧，通过。它使用相同 UDP 格式与处理管线，源文件从手机发到本机地址，不经过 Wi-Fi，用于隔离无线因素；不能替代无线链路验证。

最初用户确认保留后台后的无线测试没有通过：电脑按 30.0003 FPS 发出 10,160 帧，手机重组收到 9,608 帧、执行 9,448 帧，发生数次持续数秒的输入中断，之后能恢复，QNN 错误为零。原测试用“接收总数达到预期”识别 EOF，丢包后把收尾误记为推理停滞；现已改为末帧序号识别 EOF，完整性仍单独检查，失败也保留接收报告与本次追踪数据。运行编号校验避免误用旧结果。没有把失败的后台测试改写为通过。

测试补齐与生产服务一致的 Wi-Fi 性能锁。Android 14 起 HIGH_PERF 会映射到 LOW_LATENCY，该模式要求应用前台、屏幕点亮；仅有 CPU 唤醒锁不能证明后台无线处于同样条件。参见 [Android WifiManager 文档](https://developer.android.com/reference/android/net/wifi/WifiManager#WIFI_MODE_FULL_HIGH_PERF)。这些系统约束是复测条件的依据，不能单凭它们确定首次丢帧的唯一原因。

## 延迟

以下是结束时最近 300 帧（约 10 秒）的滚动窗口，单位毫秒，不是把全程所有窗口合并计算的百分位数。p95 表示该窗口内 95% 样本不超过该值。

| 阶段 | p50 | p95 | p99 |
| --- | ---: | ---: | ---: |
{table}

全程逐秒滚动窗口的 QNN p50 范围为 {summary['qnn_window_p50_range_us'][0]/1000:.3f}–{summary['qnn_window_p50_range_us'][1]/1000:.3f} ms。输入固定 30 FPS；不代表测得设备最高吞吐率。独立进程不驱动 HID，不依赖 Activity 生命周期停止；没有覆盖桌面采集、编码以及主机或鼠标实际接受指令的时间。

前台生产服务另完成一次 1,270 / 1,270 帧回放，结束时最近 300 帧 NPU p50 / p95 为 6.989 / 7.800 ms，手机接收到结果 17.640 / 20.942 ms。该次随后的长测在进入后台时按生产规则停止，记录为中断，不能把整次运行标记为通过。更早的一次独立测试被系统记录为 USER REQUESTED / FORCE STOP / SwipeUpClean；这些中断没有计入本次通过结果。

## 蓝牙控制调度

真机保持原有蓝牙连接，分别测量 300 次零位移指令，另有各 20 次预热；没有移动用户鼠标。同步调用与有界工作线程对照：

| 指标 | 同步基线 | 有界工作线程 |
| --- | ---: | ---: |
| 调用线程 p50 | 1.028 ms | 0.074 ms |
| 调用线程 p95 | 2.218 ms | 0.166 ms |
| Android API 完成 p50 | 1.034 ms | 2.555 ms |
| Android API 完成 p95 | 2.256 ms | 4.444 ms |

调用线程中位阻塞约减少 93%；排队/调度也增加了 API 完成时间。320 次异步请求（含预热）均被 API 接受，拒绝、取消、过期为零，最大排队 1.419 ms。收益是把 Binder 等待移出推理线程；Android API 接受不是电脑/物理鼠标确认。未实测游戏闭环中的手感或完整物理 HID 延迟。

源代码还加入指令期限和代次检查，停止/恢复/断线会作废待发送旧指令；修复控制状态被旧快照覆盖的问题，保留“始终开启”与两个侧键。Host 重复帧不再替换待发送新帧；解码器合并已就绪旧输出，过旧帧在预处理前丢弃；日志走有界异步队列，保留阻断原因与耗时。

## 模型选择与识别限制

使用原始中心裁剪输入的四模型对照，每个模型都完成 1,270 帧；下列为各自结束时滚动窗口 p50，非随机交叉试验。

| 模型 | 预处理 ms | QNN ms | 手机接收到结果 ms |
| --- | ---: | ---: | ---: |
| m416 | 1.703 | 6.540 | 17.707 |
| m320 | 6.005 | 4.118 | 17.200 |
| m640 | 17.544 | 13.789 | 38.097 |
| n640 | 17.316 | 5.350 | 29.364 |

选择 m416：它匹配当前 416×416 传输，省去缩放，保留更大的输入；m320 的端到端优势很小。此选择不等于有标注数据证明了全英雄最佳准确率。其他三个模型库已移出应用，保存在工作区外的开发归档。

提供的视频是 1280×720、30 FPS、42.33 秒的剪辑。长测为了覆盖完整画面，把视频等比缩小并填充成 416×416；生产 Host 仍发送中心 ROI，未擅自改成全屏缩小。首轮记录 {len(frames)} 帧，其中置信度 ≥0.2 的检出帧 {len(positive)}，≥0.4 的检出帧 {len(high)}。这说明该素材上检出偏少；没有标注，不能把这些比例叫准确率或召回率。较早中心裁剪的 416 回放仅 1 帧达到 0.2，因此识别本身可能造成输出断续；单纯降低阈值会引入更多特效/UI 误检。

一个原先疑似量化偏差的帧，在统一到生产 BT.709 YUV 预处理后，CPU FP32 置信度为 0.218683，手机 QNN 为 0.215241；此前 FFmpeg PNG 色彩/插值路径不同，不能据此判定量化损坏。这是局部数值核对，不是完整精度评估。

用户已选择简单头部方案。默认框中心；头部选项明确标为框上方 18% 的估计，没有新增头部模型、训练或真实头部定位保证。离线控制轨迹只检查有限检测片段上的计算，没有证明游戏反馈闭环平滑。

## 回归与证据

- 最新 Native CTest：44 / 44 通过，0 跳过；`host-build-single.log`。
- Java 行为测试：43 / 43 测试类通过，包含异步蓝牙、旧指令取消、按键叠加、日志和单模型迁移；`../java-tests.json`。
- Android release 和测试 APK 构建成功，APK v2 签名验证通过；`../apk-signature.txt`。
- 前台无线长测：`video-foreground-final2/video-headless/video-replay.json`；逐帧尾部指标与首轮检测 CSV 同目录。本地长测为 `video-local-final`，首次后台无线失败为 `video-isolated-final3`。最初直接由后台测试拉起界面没有实际进入前台，该尝试 `video-foreground-final` 也失败；复测改为 ADB 显式打开 VF，并用 Activity 采样确认状态。
- 真机蓝牙：`bluetooth-dispatch-test.json`；简明机器可读汇总：`validation-summary.json`。
- 历史崩溃修复：`../overwatch/CRASH-FIX.md`。当前长测完成不能保证任何使用条件下永不崩溃。

私人视频与历史大体积诊断不进入源码 ZIP。完整手机及桌面游戏闭环仍需实际操作验证，不能把上述自动测试写成“所有场景已经不卡顿”。
'''
(audit/'RESULTS.md').write_text(text,encoding='utf-8')
print(json.dumps(summary,ensure_ascii=False,indent=2))
