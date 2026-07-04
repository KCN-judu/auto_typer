# Auto Typer 项目快速索引

这个仓库是一个 ESP32-S3 自动打字机项目，包含固件、桌面控制台、共享协议和硬件资料。后续修改时优先从本文定位入口，再去读对应源码。

## 工作规则

- 按仓库全局要求，运行 shell 命令时使用 `rtk` 前缀，例如 `rtk npm run desktop:typecheck`。
- 不要把硬件动作的“命令发送成功”误认为“动作已经完成”。当前固件大部分运动控制只确认 CAN 帧发送成功，未确认电机实际到位。
- 这里的运动状态主要由本地软件估算和累计步数维护，硬件丢步、堵转、同步失败或等待时间估算不足都会让软件状态和真实机械位置产生偏差。

## 目录索引

- `src/auto_typer/`：主固件。目标是 ESP32-S3，控制 OLED、PCA9685 舵机、Emm_V5.0 CAN 电机，使用编译期 `Secrets.h` 静态 Wi-Fi 凭据，并提供基于 TCP 的远程 group 执行。
- `apps/desktop/`：Electron + Vite + React 桌面控制台，用于连接 ESP32、创建任务、调试电机/舵机和维护键位映射。
- `shared/protocol/`：桌面端和固件 TCP NDJSON 控制协议的 TypeScript 类型源。
- `src/test/test/`：硬件连通性测试草图，覆盖 OLED、PCA9685、舵机和 CAN 电机。
- `materials/`：电机、PCB、外壳、说明书和供应商例程资料。

## 关键入口

- `src/auto_typer/auto_typer.ino`：薄 Arduino 生命周期入口。`setup()` / `loop()` 委托给 `AutoTyperFirmware.h` 的公开固件 API。
- `src/auto_typer/network/StaticWifiConnector.h`：静态 Wi-Fi 连接器。读取 `config/Secrets.h` 中的 `AUTO_TYPER_WIFI_SSID` / `AUTO_TYPER_WIFI_PASSWORD`，在 Wi-Fi 就绪后放行 TCP server 启动。
- `src/auto_typer/auto_typer_config.h`：默认硬件配置、运动参数、舵机参数、Wi-Fi、CAN 和机器拓扑。
- `src/auto_typer/auto_typer_types.h`：核心枚举和结构体，包括 `JobState`、`DeviceMode`、`TypingStepKind`、运动 profile 和任务快照。
- `src/auto_typer/typing_logic.h`：纯规划逻辑，把文本和 keymap 转成 `TypingPlan`。
- `src/auto_typer/auto_typer_runtime.h`：固件应用状态机和动作执行逻辑，是状态、运动、回位和调试入口的核心。
- `src/auto_typer/hal_emm_can_motion.h`：Emm_V5.0 CAN 命令封装。这里只负责组包和 `twai_transmit()`。
- `src/auto_typer/transport/GroupCommandServer.h`：TCP `7777` NDJSON group command 入口，用于桌面端分组下发远程运动、遥测、取消、探测和故障恢复。
- `src/auto_typer/can/`：CAN TX 队列、RX 任务、Emm_V5.0 事件解析、运动反馈缓存和协议 trace。
- `shared/protocol/auto-typer-protocol.ts`：桌面端协议类型和路由常量。
- `apps/desktop/src/ui/App.tsx`：桌面 UI 主组件。
- `apps/desktop/src/domain/groupStreamClient.ts`：桌面端 TCP group command 客户端。

## 常用命令

- `rtk npm run desktop:dev`：启动桌面端开发环境。
- `rtk npm run desktop:build`：构建桌面端。
- `rtk npm run desktop:typecheck`：桌面端类型检查。

## 固件主流程

启动流程在 `AutoTyperApplication::setup()`：

1. 构建或加载飞宇 200 keymap。
2. 初始化 I2C OLED。
3. 初始化 PCA9685 舵机驱动。
4. 等待 2000 ms。
5. 初始化 TWAI/CAN。
6. 对 X、Y 左、Y 右、走纸电机依次设置闭环控制模式并使能，每条命令之间固定 `delay(80)`。
7. 执行初始走纸回车 `executeInitialLineFeed()`。
8. 显示 Idle。

循环流程在 `auto_typer.ino`：

1. `gWifi.tick()` 维护静态 Wi-Fi 连接和有限重试日志。
2. Wi-Fi 连通后 `gGroupServer.tick()` 处理 TCP group command，接收远程 group、发送执行事件并推送遥测。
3. `gApp.tick()` 推进当前运行时状态；如果有 queued 的本地任务或远程 group，就驱动同一个执行器状态机继续运行。
4. `delay(1)`。

注意：执行链路里仍有阻塞式动作和等待。长动作期间新的 TCP 输入、事件发送和控制响应都只能在返回主循环后继续推进。

## 状态逻辑

任务状态枚举在 `JobState`：

- `None`：初始无任务。
- `Queued`：本地文本任务规划成功，或远程 `exec_group` 成功入队后进入。
- `Running`：`tick()` 启动执行器后进入运行。
- `Completed`：本地任务执行完成，或远程打印流在 `finishRemoteTask()` 后收尾完成。
- `Cancelled`：`cancelCurrentJob()` 成功后进入。取消会停止当前执行流；远程 group 会额外产出对应的 cancelled 终态事件。
- `Failed`：规划失败、执行失败、收尾失败或急停后进入。

本地文本任务主线：

```text
None/Completed/Cancelled/Failed
  -> submitTextJob() plan ok
  -> Queued
  -> tick()
  -> Running
  -> executePlan() + returnToInitialPosition()
  -> Completed
```

失败和中断路径：

- `submitTextJob()` 如果 `planText()` 返回 `KeyNotFound` 或 `PlanFull`，直接进入 `Failed` 并显示 Error。
- `executePlan()` 中任一步命令返回 false，进入 `Failed`，设置 `faulted_ = true`。
- `returnToInitialPosition()` 失败，进入 `Failed`，设置 `faulted_ = true`。
- `emergencyStop()` 总是发送停止所有电机命令、释放舵机、进入 `Failed`，设置 `faulted_ = true`。
- `cancelCurrentJob()` 只接受 `Queued` 或 `Running`，成功后进入 `Cancelled`，不设置 `faulted_`。

远程 group 主线：

```text
TCP hello
  -> exec_group
  -> submitRemoteGroup() accepted
  -> JobState::Queued
  -> tick()
  -> JobState::Running
  -> group_started / block_started / block_done / group_done / group_final
  -> finish_task
  -> JobState::Completed
```

注意：

- 单个 `exec_group` 的完成终态由 `group_final` 表示。
- 整个远程打印任务的 UI 收尾由 `finish_task` 触发，不等同于单个 group 完成。
- `group_accepted` 只表示 admission 成功，不表示机械开始动作或已经完成。

设备模式 `DeviceMode` 是派生状态：

- `faulted_ == true` 时返回 `Faulted`。
- `JobState::Queued` 或 `JobState::Running` 时返回 `Running`。
- 其他情况返回 `Idle`。
- `DeviceMode::Debug` 目前只在类型中存在，没有运行时代码返回它。

当前固件对外控制面只保留 TCP `7777` NDJSON group command stream。它承载三类语义：

- 控制命令：`exec_group`、`finish_task`、`cancel`
- 故障与维护：`reset_fault`、`release_line_feed_origin`、`probe`、`press_diag_m5`
- 观测输出：`status`、`telemetry`、`motor_event`、`motor_state_update`、`group_*`

## 打字计划逻辑

`planText()` 把输入文本转换成最多 256 步的 `TypingPlan`：

- 初始先追加 `Release(homePoint, servo.releaseMs)`。
- 普通字符会先规范化大小写，然后在 keymap 中查找坐标。
- 每个普通字符追加：

```text
Release(current, servo.releaseMs)
MoveTo(target, 0)
Wait(target, servo.settleMs)
Press(target, servo.pressMs)
Release(target, servo.releaseMs)
CharacterRelease(target, 0)
```

- 换行符 `\n` 或 `\r\n` 追加：

```text
Release(current, servo.releaseMs)
LineFeed(current, 0)
```

- 换行后规划层把 `current.xMm` 重置为 `homePoint.xMm`。
- 找不到字符时返回 `PlanStatus::KeyNotFound` 并记录 `failedKey`。
- 步数超过 `TypingPlan.steps[256]` 时返回 `PlanStatus::PlanFull`。

## 运动时间逻辑

坐标到步数：

- `stepsPerMm = stepsPerRev / (beltPitchMm * pulleyTeeth)`。
- 默认配置为 `stepsPerRev=3200`、`beltPitchMm=2.0`、`pulleyTeeth=20`，所以默认 `stepsPerMm=80`。
- `mmToMotorSteps()` 使用 `fabs(deltaMm) * stepsPerMm`，再加 `0.5` 转成整数步。
- X 轴 delta 是 `target.xMm - current.xMm`。
- Y 轴 delta 是 `current.yMm - target.yMm`，方向定义和 X 轴相反。

运动等待：

- 所有软件等待统一走 `waitForMove(steps, rpm, settleMs)`。
- `waitForMove()` 实际调用：

```text
delay(moveDurationMs(steps, rpm) + settleMs)
```

- `moveDurationMs()` 公式：

```text
ceil(steps * 60000 / (rpm * stepsPerRev))
```

- 如果 `steps == 0`、`rpm == 0` 或 `stepsPerRev == 0`，运动时间按 0 ms 处理。
- 这个公式只按恒速估算，不显式计算加减速段；`acceleration` 只发送给电机驱动，不参与等待时间计算。

默认运动参数：

- X 常规移动：`rpm=800`、`acceleration=10`、`settleMs=120`。
- Y 常规移动：`rpm=800`、`acceleration=10`、`settleMs=120`。
- X 保守回位：`rpm=200`、`acceleration=3`、`errorSteps=150`、`settleMs=180`。
- Y 保守回位：`rpm=200`、`acceleration=3`、`errorSteps=100`、`settleMs=180`。
- 走纸：`rpm=500`、`acceleration=10`、`returnTotalSteps=16440`、`returnReleaseSteps=6400`、`characterReleaseSteps=180`、`settleMs=400`、`characterReleaseSettleMs=80`。
- 舵机：`releaseMs=200`、`pressMs=200`、`settleMs=80`。

轴运动执行：

- X 轴：`moveXMotor()` 发送相对运动命令，更新本地 X 累计步数，然后按估算时间等待。
- Y 轴：`moveYMotorGroup()` 向左右 Y 电机发送同步相对运动命令，右电机方向取反，然后分别发送同步触发；调用方通常随后调用 `waitForMove()`。
- 走纸：`moveLineFeedMotor()` 发送相对运动命令，更新本地走纸累计步数，然后按估算时间等待。
- `executeMove()` 先执行 X，再执行 Y，不是 X/Y 同时插补运动。

回位逻辑：

- `submitTextJob()` 开始时调用 `captureJobStartMotorPositions()`，记录 X、Y、走纸本地累计步数。
- 任务结束后 `returnToInitialPosition()` 会释放舵机，然后保守回 X 和 Y。
- 保守回位步数为 `abs(current - jobStart) - errorSteps`，小于等于误差步数时不回。
- `returnLineFeedMotorToJobStart()` 存在，但当前任务完成路径没有调用它。

## 没有等待硬件确认、直接发送的风险点

CAN 层风险：

- `EmmCanMotionHal::sendCommand()` 只检查 `ready_` 和 `twai_transmit(&message, pdMS_TO_TICKS(100))` 是否成功。
- `twai_transmit()` 成功只代表 CAN 帧成功交给 TWAI 发送队列或发送路径，不代表电机驱动器已经执行、完成、到位或没有报警。
- 当前代码没有读取 Emm_V5.0 的实时位置、实时转速、状态标志位、堵转/报警状态或回零状态。

运动命令风险：

- `moveXMotor()`、`moveLineFeedMotor()` 发送命令后直接按公式 `delay()`，没有硬件到位确认。
- `moveYMotorGroup()` 只发送左右电机同步运动命令和同步触发命令，本身不等待；`executeMove()`、`conservativeReturnYMotorGroup()` 会在外层等待，但调试入口不额外等待。
- `debugMotorMoveRelative()` 对 Y 组会调用 `moveYMotorGroup()` 后立即返回；这可能早于真实 Y 轴运动完成。
- `debugMotorMoveRelative()` 对未知 motor id 直接调用 `motion_.moveRelative()`，没有等待、没有本地位置跟踪、没有到位确认。
- `enableMotor()`、`disableMotor()`、`stopNow()`、`triggerSynchronousMotion()` 都是发送即返回，不读取驱动器确认。
- `prepareMotors()` 设置闭环模式和使能电机后只固定等待 `80 ms`，没有确认模式切换或使能状态。

状态和位置风险：

- `currentPoint_` 在 `executeMove()` 成功返回后直接设为目标点；成功条件只是发送命令和等待估算时间成功，不代表机械真实到位。
- X/Y/走纸位置由 `trackSignedMotorMove()` 本地累计。如果电机没有执行、丢步、堵转、同步触发失败或方向配置错误，本地累计会继续变化并污染后续回位。
- `faulted_` 被设置为 true 后，需要通过 TCP group command `reset_fault` 尝试恢复；恢复是否成功取决于 CAN/执行器状态。
- 任务执行是阻塞式 `delay()`，运行中 TCP 取消、故障恢复或其他控制命令都无法在长时间 delay 内被及时处理。

同步运动风险：

- Y 组用特殊 motor id `23` 表示逻辑组，实际控制电机 `2` 和 `3`。
- 左右 Y 电机方向相反：右侧使用 `invertDirection(direction)`。
- 同步模式下两台电机先接收 `moveRelative(..., sync=true)`，再分别发送 `triggerSynchronousMotion()`。代码没有确认两台驱动器都已缓存好命令，也没有确认同步触发后两台都开始运动。

舵机风险：

- 舵机 HAL 使用固定 dwell delay，不能确认按键是否真的压下或释放。
- `debugServo()` 在 queued/running 时拒绝调试命令，但任务执行中的 `servo_.press()` / `servo_.release()` 仍是阻塞等待，不带外部反馈。

## 协议和桌面端注意事项

- `shared/protocol/auto-typer-protocol.ts` 是桌面端类型源；ESP32 端手写 JSON 字段名，需要人工保持一致。
- 桌面端主要通过 TCP `7777` 的 NDJSON group command stream 获取分组执行事件和遥测。客户端必须先发 `hello`，再发 `exec_group`、`finish_task`、`cancel`、`reset_fault`、`release_line_feed_origin`、`probe`、`press_diag_m5` 或 `ping`。
- `status` / `telemetry` 反映的是运行时快照；`group_done` / `group_final` 反映的是单个 group 的执行结果；两者不要混用。
- `motor_event`、`motor_state_update` 和 `telemetry_overflow` 是 CAN 观测通道，不是动作完成确认。
- 桌面端 debug 页在 `status.mode !== "running"` 时允许调试；固件端也会拒绝 queued/running 下的电机和舵机调试命令。

## 测试草图

`src/test/test/` 是独立 Arduino 测试草图：

- `test.ino`：Arduino 生命周期入口。
- `app_config.h`：测试用引脚和协议配置。
- `app_logic.h`：测试计划和状态映射。
- `app_runtime.h`：轮转执行 I2C、舵机和 CAN 测试。
- `hal_i2c.h`、`hal_display.h`、`hal_servo.h`、`hal_can_motor.h`：测试用硬件 HAL。

测试目标：

- MCU：ESP32-S3。
- I2C：SDA GPIO6、SCL GPIO7。
- CAN/TWAI：TX GPIO4、RX GPIO5、500 kbps。
- OLED：SH1106 128x32，优先 `0x3C`，fallback `0x3D`。
- PCA9685：`0x40`。
- Emm_V5.0 CAN 电机：地址 `1`。
