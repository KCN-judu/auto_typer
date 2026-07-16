@/Users/kcn/.codex/RTK.md

--- project-doc ---

# Auto Typer 项目快速索引

本仓库包含 ESP32-S3 主固件、Electron 桌面控制台、共享 TCP 协议、Arduino 打包工具、独立硬件测试草图和硬件资料。修改前先从本索引定位源码入口，并以代码而不是历史文档为准。

## 工作规则

- 文件命名约定只约束文件名和路径名，不约束变量、函数、类型或类。桌面端 TypeScript 文件使用小驼峰，例如 `motionProtocolClient.ts`；固件 C++ 文件使用大驼峰，例如 `MotionProtocolServer.h`。
- 不要把 CAN 发送成功、命令 ACK 或 `block_ack` 当成机械动作完成。已接收原子块只有对应的 `block_result` 才是终态。
- 当前协议只接受绝对目标脉冲。桌面端维护累计逻辑位置；设备断电、急停、堵转或机构被人工移动后，计划位置可能与机械位置不一致。
- 每次上电前必须人工把完整机构放回定义的机械原点。固件的 pulse clear 只建立电气零点，不执行机械回零。

## 目录索引

- `src/auto_typer/`：主固件，控制 OLED 和五台 Emm_V5.0 CAN 电机，提供 SoftAP 配网和 TCP `7777` 原子运动协议。
- `apps/desktop/`：Electron + Vite + React 控制台，负责配网、连接、键位映射、文本规划、绝对脉冲编码、打印和恢复操作。
- `shared/protocol/`：桌面与固件共同遵守的 TCP NDJSON v1 TypeScript 类型源。
- `tools/`：固件源码编译、预编译 Arduino 库、离线 Arduino 工作区生成与验证脚本。
- `src/test/test/`：独立硬件连通性测试草图，不是生产固件入口。
- `src/test/docs/`：Typst 烧录与使用教程及真实界面截图。
- `materials/`：电机、PCB、外壳、说明书和供应商资料。

## 关键入口

- `src/auto_typer/auto_typer.ino`：Arduino 生命周期入口，只调用无参 `autoTyperSetup()` 和 `autoTyperLoop()`。
- `src/auto_typer/AutoTyperFirmware.cpp`：组装 Wi-Fi、CAN、反馈缓存、运行时和 TCP server；`setup()`/`loop()` 的实际委托目标。
- `src/auto_typer/network/ProvisioningWifiConnector.h` 与 `src/auto_typer/ProvisioningWifiConnector.cpp`：SoftAP + STA 配网状态机和 HTTP `/api/status`、`/api/provision`、`/api/finish`。
- `src/auto_typer/auto_typer_config.h`：设备 ID、CAN 引脚、机器拓扑、运动参数和绝对脉冲目标默认值。
- `src/auto_typer/auto_typer_types.h`：任务、设备、运动、反馈和远程动作类型。
- `src/auto_typer/auto_typer_runtime.h`：启动清零、任务状态、readiness、故障、原子块转换和运行时事件。
- `src/auto_typer/motion/MotionExecutor.h`：动作状态机、ACK/脉冲/速度监督、取消和急停。
- `src/auto_typer/transport/MotionProtocolParser.h`：`execute_block` 的结构与数值校验。
- `src/auto_typer/transport/MotionProtocolServer.h`：TCP `7777` NDJSON v1 会话、命令、快照与终态事件。
- `src/auto_typer/protocol/EmmV5CommandCodec.h`：Emm_V5.0 CAN 帧编码；位置命令使用绝对脉冲目标。
- `src/auto_typer/can/`：TWAI、TX 队列、RX 任务、事件解析、反馈缓存和协议 trace。
- `shared/protocol/protocolTypes.ts`：唯一共享 TypeScript 协议类型源。
- `apps/desktop/electron/deviceLink.ts`：Electron TCP 链路。
- `apps/desktop/electron/runtime/provisioning_http.ts`：桌面端 SoftAP HTTP 客户端。
- `apps/desktop/src/domain/planner/motionBlockPlanner.ts`：文本到动作块规划。
- `apps/desktop/src/domain/planner/absoluteMotionEncoder.ts`：逻辑位置到 M1-M5 绝对脉冲目标编码。
- `apps/desktop/src/domain/runtime/executor/motionProtocolClient.ts`：握手、快照、控制命令和 block 客户端。
- `apps/desktop/src/ui/hooks/usePrintTaskController.ts`：打印、普通取消回零、急停和内部调试操作编排。
- `apps/desktop/src/ui/App.tsx`：桌面 UI 主组件。

## 常用命令

- `npm run desktop:dev`：启动 Electron 开发环境。
- `npm run desktop:typecheck`：检查 renderer 与 Electron TypeScript。
- `npm run desktop:test`：运行绝对运动编码和设备链路测试。
- `npm run desktop:build`：类型检查并构建桌面端。
- `npm run firmware:test`：运行固件宿主侧回归测试。
- `npm run firmware:compile`：编译主固件源码。
- `npm run firmware:package`：生成预编译 `AutoTyperCore`。
- `npm run firmware:package:upload`：使用环境变量中的 Wi-Fi 凭据编译并上传。
- `npm run firmware:workspace`：生成 macOS 离线 Arduino 工作区。
- `npm run firmware:workspace:verify`：验证板型暴露和 starter sketch 编译。

## 启动与配网事实

1. 无参 `autoTyperSetup()` 启动串口、调用 `gWifi.begin()`，再执行 `gApp.setup()`；固件没有编译期或 sketch 注入的 Wi-Fi 凭据接口。
2. Wi-Fi 进入 `WIFI_AP_STA`，建立 SSID `wifi-setup`、密码 `admin123`、信道 `6` 的 SoftAP，并在端口 `80` 启动配网 HTTP server。
3. 固件初始化 OLED、TWAI 和 CAN 队列；向 M1-M5 发送闭环模式/使能配置，清除五台电机脉冲位置，并验证 clear ACK 与近零输入脉冲。
4. `/api/provision` 接收 SSID/密码并触发 STA 连接；连接超时为 `20000 ms`。
5. `/api/finish` 只在 STA 已连接时成功；响应后约 `500 ms` 关闭 SoftAP，并通过 `consumeTcpReady()` 放行 TCP server 启动。
6. TCP server 同时只允许一个 client；连接后 `3000 ms` 内未完成 handshake 会断开。

Wi-Fi 必须通过 SoftAP `/api/provision` 直接配网。不要重新引入 `Secrets.h`、`FirmwareConfig`、Wi-Fi 宏或上传环境变量；修改配网行为时必须同步 README 与 Typst 教程。

## 协议事实

客户端在 handshake 后可发送：

- `get_snapshot`
- `heartbeat`
- `execute_block`
- `cancel`
- `finish_task`
- `emergency_stop`
- `reset_fault`

设备响应/事件包括：

- `handshake_ack`、`snapshot`、`heartbeat_ack`
- `block_ack`、`block_result`
- `cancel_result`、`finish_task_result`
- `emergency_stop_result`、`reset_fault_result`
- `fault`、`protocol_error`

限制：

- 单行最大 `8192` bytes。
- `policy.maxRuntimeMs` 最大 `30000 ms`。
- 单动作 `timeoutMs` 最大 `10000 ms`。
- 固件只有一个活动 block 槽；下一 block 必须等待当前 `block_result`。
- block 允许单个 `motor_move` 或 `wait`；多动作 block 只允许 M1/M2/M3 的 XY 同步组合。
- `get_snapshot` 是任务、CAN、motor readiness 和 fault 的唯一显式观测路径；固件不主动推送 telemetry/motor state。

## 位置与任务事实

桌面逻辑位置为 `{x, y, l, z}`，物理映射为：

- M1 = `x`
- M2 = `-y`
- M3 = `y`
- M4 = `l`
- M5 = `z`

当前默认目标/参数：

- 坐标换算 `80 steps/mm`。
- XY 打印 `1600 RPM`、`accelRaw=128`、`timeoutMs=10000`。
- M5 按压 `3000 RPM`、`accelRaw=255`；按下 `-2700`，释放 `0`。
- M4 走纸到位是固定绝对序列 `16400 -> 10000`。
- 普通字符释放会让 M4 在当前目标基础上累计 `-180` pulses。

当前桌面限制：`lineFeedPrimeRequired_` 初始为 true，只有包含 M4 rest target `10000` 的成功动作计划才会清除。`usePrintTaskController()` 返回了 `runLineFeedHome()`，但 `App.tsx` 没有把它绑定到可见控件，因此当前 UI 在全新上电状态不能独立完成首次走纸到位。修改 UI 或文档时不要假设该入口已经存在。

远程 block 状态主线：

```text
execute_block
  -> 完整校验
  -> block_ack + Queued
  -> tick() 启动 MotionExecutor + Running
  -> ACK/输入脉冲/速度/timeout 监督
  -> block_result(done | failed | cancelled)
```

`finish_task` 只在没有 queued/planning/running/cancelling/active block 时接受；成功后 OLED 显示 `Complete` `3000 ms`，然后回到 `Idle`。

## 取消与故障事实

- 桌面普通取消是协作式的：等待当前 block 完成，丢弃未发送 block，再提交 XY→0、M4→16400→10000、M5→0 的绝对回零序列，不发送 `finish_task`。
- 协议 `cancel` 与断线只取消 `Queued` block；Running block 不被普通取消中断。
- `emergency_stop` 清空 CAN 队列、停止/失能/解锁五台电机、锁存 fault、使任务失败并显示 `Error`。
- `reset_fault` 清理执行器和 CAN fault 后重新探测五台电机；CAN 或 motor readiness 未恢复时继续保持 fault。
- 急停或异常停止后不能假设绝对坐标仍可信，必须先人工检查并恢复机械原点。

## 文档维护

- 操作教程：`src/test/docs/auto_typer_flash_and_usage_guide.typ`。
- 协议说明：`shared/protocol/README.md`。
- 独立测试草图说明：`src/test/test/README.md`。
- 协议、配网、板型、目标脉冲、UI 流程或安全语义变化时，同步更新对应文档和截图。
