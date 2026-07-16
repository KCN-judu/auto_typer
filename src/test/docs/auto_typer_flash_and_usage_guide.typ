#import "template.typ": theme, title-block, info-box, note-box, command, screenshot

#theme

#title-block(
  [Auto Typer 固件烧录与桌面端使用教程],
  subtitle: [ESP32-S3 固件、SoftAP 配网、TCP 连接、打印与故障恢复],
  version: [V1.0],
  date: [2026-07-10],
)

#v(8pt)

#info-box([文档边界])[
  本教程以仓库当前代码为准，覆盖主固件与 Electron 桌面控制台。旧版 OLED、舵机和单电机“连通性测试报告”已废弃；独立测试草图仍保留在 `src/test/test/`，但不再作为整机烧录入口。
]

#v(6pt)

#note-box([先读安全事项], warning: true)[
  - 上电前把 X、Y、走纸和按压机构手动放到定义的机械原点，并确保运动范围内无人手、工具和松动物料。
  - 固件启动会把五台 Emm_V5.0 电机的当前位置清为脉冲零点；这是坐标清零，不是机械回零。
  - `block_ack` 只表示原子动作块通过校验并占用执行槽。只有对应的 `block_result(status: "done")` 才表示固件监督到该动作块完成。
  - 普通“取消”不会打断已经运行的动作块；需要立即停止时使用“急停”。急停后必须检查机构，再执行“复位并清故障”。
]

= 系统与网络流程

当前整机由两个程序组成：

- *固件端*：ESP32-S3 驱动 OLED、CAN 电机和运动执行器，提供 SoftAP 配网 HTTP 接口与 TCP `7777` 原子运动协议。
- *桌面端*：Electron 应用负责键位表、文本规划、毫米到绝对脉冲目标的转换、原子动作块排序以及任务操作界面。

正常启动链路如下：

#align(center)[
  #box(fill: rgb("#EEF4FF"), inset: 9pt, radius: 5pt)[
    上电并清零电机坐标 → 连接 `wifi-setup` → 写入局域网凭据 → 获取设备 IP → TCP `7777` 握手 → 走纸到位 → 开始打印
  ]
]

固件上电后建立以下配网网络：

#table(
  columns: (4cm, 5cm, 5.5cm),
  inset: 6pt,
  fill: (x, y) => if y == 0 { rgb("#EAF1FF") } else { white },
  [项目], [当前值], [用途],
  [SoftAP 名称], [`wifi-setup`], [电脑临时连接到设备],
  [SoftAP 密码], [`admin123`], [连接配网网络],
  [HTTP 地址], [`http://192.168.4.1`], [状态、配网与收尾接口],
  [控制端口], [`7777`], [TCP NDJSON v1 原子运动协议],
)

= 固件烧录

== 推荐方式：离线 Arduino 工作区

仓库可以生成一个面向 macOS 的离线工作区。它包含预编译 `AutoTyperCore`、ESP32 工具链、自定义板型和可直接打开的 starter sketch；本机仍需已安装 Arduino IDE。

在仓库根目录执行：

#command("npm install\nnpm run firmware:workspace\nnpm run firmware:workspace:verify")

生成位置为：

```text
dist/arduino/AutoTyperWorkspace/
```

依次完成：

+ 打开 `Launch AutoTyper Arduino.command`。
+ 连接 ESP32-S3 的 USB 数据线。
+ 在 Arduino IDE 中选择设备串口。
+ 选择工作区唯一支持的板型 `AutoTyper ESP32S3 Dev Module`。
+ 确认机械处于定义原点并已清空运动范围，然后点击“上传”。
+ 烧录完成后连接 `wifi-setup`，必须通过桌面端 SoftAP 页面直接写入目标 Wi-Fi；固件不再支持 `Secrets.h`、编译期宏或 sketch 注入凭据。

#screenshot(
  "assets/tutorial/arduinoFirmware.jpeg",
  [仓库生成的 `AutoTyperProvisioning` 草图在 Arduino IDE 2.3.5 中的真实界面。截图尚未选择开发板；烧录前必须完成板型和串口选择。],
)

== 手动板型参数

不使用离线工作区时，可从 `dist/arduino/AutoTyperCore/` 安装预编译库，并选择以下参数：

#table(
  columns: (5cm, 9.5cm),
  inset: 6pt,
  fill: (x, y) => if y == 0 { rgb("#EAF1FF") } else { white },
  [Arduino IDE 项目], [取值],
  [Board], [`ESP32S3 Dev Module`],
  [Flash Size], [`16MB (128Mb)`],
  [Flash Mode], [`OPI 80MHz`],
  [PSRAM], [`OPI PSRAM`],
  [Partition Scheme], [`16M Flash (3MB APP/9.9MB FATFS)`],
)

== 命令行编译与上传

源码编译、预编译库验证和上传分别使用：

#command("npm run firmware:compile\nnpm run firmware:package\nnpm run firmware:package:verify")

上传 provisioning-only 示例：

#command("npm run firmware:package:upload -- --port=/dev/cu.usbmodemXXXX")

省略 `--port` 时，脚本按 `usbmodem`、`ttyACM`、`ttyUSB`、其他 `/dev/cu.*` 的顺序自动选择可能的 ESP32-S3 串口。

== 首次上电判定

固件启动顺序是：初始化 OLED → 初始化 TWAI/CAN → 设置五台电机闭环模式 → 使能 → 清除五台电机脉冲位置 → 探测反馈。任何关键步骤失败都会锁存故障并显示 `Error`。

当前串口速率为 `115200`。串口调试宏默认关闭大部分日志，但 Wi-Fi 连接成功后仍会输出：

```text
[wifi] connected ssid=<SSID> ip=<DEVICE_IP>
```

= 首次配网与 TCP 连接

== 启动桌面端

开发环境运行：

#command("npm run desktop:dev")

发布构建使用：

#command("npm run desktop:pack\nnpm run desktop:dist:mac\nnpm run desktop:dist:win")

`desktop:pack` 生成未打包应用目录；macOS 分发任务生成 `.dmg` 与 `.zip`，Windows 分发任务生成 portable 包。仓库默认未启用代码签名。

== 通过 SoftAP 配网

+ 在电脑 Wi-Fi 菜单连接 `wifi-setup`，密码为 `admin123`。
+ 打开桌面端“设置”。
+ 在“Wi-Fi 配网”中填写目标局域网的 SSID 和密码，点击“开始配网”。
+ 桌面端向 `192.168.4.1/api/provision` 写入凭据，并每 `2.5 s` 查询状态。
+ 设备连接目标网络后，桌面端调用 `/api/finish` 关闭 SoftAP，并用返回的设备 IP 自动尝试 TCP 连接：先等待 `3 s`，最多尝试 `10` 次，每次间隔 `2 s`。

#screenshot(
  "assets/tutorial/desktopSettings.jpeg",
  [桌面端“设备与网络”真实界面。上方可直接填写设备 IP 和端口 `7777`；下方用于 SoftAP 首次配网。截图处于离线状态。],
)

如果设备已经配网，也可以直接在“TCP 设备”区域填写设备局域网 IP 和端口 `7777`，点击“连接设备”。连接成功后桌面端先完成 `handshake`，再请求 `get_snapshot`；只有快照成功才显示连接完成。

= 打印前检查

开始打印按钮需要同时满足：

- TCP 已连接；
- `motionReady` 与 `pressReady` 均为真；
- `lineFeedPrimeRequired` 为假；
- 设备没有故障，也没有 queued/running/cancelling 任务；
- 键位映射校验没有问题；
- 输入文本非空。

#note-box([当前桌面端限制], warning: true)[
  首次上电后 `lineFeedPrimeRequired` 初始为真，界面显示“走纸机构：需到位”，因此“开始打印”保持禁用。`usePrintTaskController.ts` 已实现 `runLineFeedHome()`，但当前 `App.tsx` 没有把该操作接到可见按钮；所以当前桌面 UI 不能独立完成首次走纸到位。只有 M4 经过受监督的绝对序列 `16400 → 10000` 后，固件才会清除此标志。不要通过伪造快照或直接绕过 readiness 启动打印；发布给操作员前应先补齐并验证这个 UI 操作入口。
]

== 检查键位映射

“映射”页展示飞宇 200 键位坐标、机械坐标方向和当前打印头位置。当前内置键位集合包含数字、字母、符号、空格、`CapsLock` 和 `LShift`；文本中的字符必须能够映射到其中的物理按键。

#screenshot(
  "assets/tutorial/desktopKeymap.jpeg",
  [桌面端“键位映射”真实界面。可搜索、悬停或点击按键查看坐标；截图来自当前应用的离线展示状态。],
)

= 执行打印任务

+ 打开“打印”页，在“打印内容”中输入文本。
+ 确认顶部的运动系统、按压电机、走纸机构和当前任务均已就绪；当前版本还需先解决上节所述的走纸到位 UI 入口缺失。
+ 点击“开始打印”。
+ 观察右侧当前动作、动作块计数、完成数和运行记录。
+ 等待所有动作块返回 `done`；队列清空后桌面端发送 `finish_task`，固件 OLED 显示 `Complete` 约 `3 s` 后回到 `Idle`。

#screenshot(
  "assets/tutorial/desktopPrint.jpeg",
  [桌面端“打印”真实界面。截图未连接设备，因此“开始打印”不可用；离线快照中的绿色项目不能替代连接后的最新设备快照。],
)

桌面规划器维护逻辑位置 `{x, y, l, z}`，并输出以下绝对脉冲目标：

#table(
  columns: (2.5cm, 4cm, 8cm),
  inset: 6pt,
  fill: (x, y) => if y == 0 { rgb("#EAF1FF") } else { white },
  [电机], [逻辑目标], [当前用途],
  [M1], [`x`], [X 轴],
  [M2], [`-y`], [Y 左],
  [M3], [`y`], [Y 右，与 M2 方向相反],
  [M4], [`l`], [走纸；到位序列 `16400 → 10000`],
  [M5], [`z`], [按压；按下 `-2700`，释放 `0`],
)

= 取消、急停与恢复

== 普通取消

点击“取消”后，桌面端等待当前已确认 block 完成，丢弃尚未发送的后续 block，然后提交绝对回零序列：XY → `0`，M4 → `16400 → 10000`，M5 → `0`。普通取消不会发送 `finish_task`。

协议级 `cancel` 只可能取消尚未开始运行的 queued block；running block 返回 `ok: false`。TCP 断线采用相同的 queued-only 边界。

== 急停

“急停”会中止桌面等待，并要求固件清空排队 CAN 工作、停止五台电机、失能五台电机、发送清堵转保护命令并锁存故障。界面进入“需要复位”，OLED 保持 `Error`。

#note-box([急停后的处理], warning: true)[
  急停响应成功不代表机构已经安全回到原点。先断开动力或确认机构静止，检查卡滞与坐标，再手动恢复机械原点。只有现场检查完成后，才点击“复位并清故障”；恢复请求还会重新探测电机 readiness，探测失败会继续保持故障。
]

= 常见问题

#table(
  columns: (5cm, 9.5cm),
  inset: 6pt,
  fill: (x, y) => if y == 0 { rgb("#EAF1FF") } else { white },
  [现象], [检查顺序],
  [`wifi-setup` 不可见], [确认固件已启动；重新上电；确认没有在成功配网后调用 `/api/finish` 关闭 SoftAP。],
  [配网停在 `FAILED`], [查看 `reason`：常见值为 `NO_SSID`、`AUTH_FAIL`、`CONNECTION_LOST`、`DISCONNECTED` 或 `TIMEOUT`。],
  [TCP 连接失败], [确认电脑与设备处于同一局域网；确认设备 IP；确认端口为 `7777`；确认没有另一 TCP 客户端占用唯一连接。],
  [连接后不能打印], [刷新设备快照，依次检查 `motionReady`、`pressReady`、走纸到位、fault 和键位表。],
  [`block_ack` 后没有完成], [继续等待匹配的 `block_result`；超时或 `failed` 时不要自动重发不确定请求，先获取快照并处理故障。],
  [急停后仍不可操作], [完成机械检查和人工回原点，再执行“复位并清故障”；若 readiness 未恢复，检查 CAN 与对应电机反馈。],
)

= 维护者验证清单

修改固件、协议或桌面行为后，至少执行：

#command("npm run firmware:test\nnpm run firmware:compile\nnpm run desktop:typecheck\nnpm run desktop:test\nnpm run desktop:build")

协议字段以 `shared/protocol/protocolTypes.ts` 为唯一 TypeScript 类型源；固件在 `MotionProtocolServer.h` 中手写 JSON 字段。两端变化必须同步更新本教程与 `shared/protocol/README.md`。
