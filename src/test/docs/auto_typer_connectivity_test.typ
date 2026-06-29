#import "template.typ": theme, title-block, info-box

#theme

#let pin-table = table(
  columns: (2.2cm, 2.2cm, 3cm, 6.2cm),
  inset: 6pt,
  fill: (x, y) => if y == 0 { rgb("#EAF1FF") } else { white },
  [模块], [接口], [主控板连接], [备注],
  [OLED], [SDA], [GPIO6], [与 PCA9685 共用 I2C 总线],
  [OLED], [SCL], [GPIO7], [与 PCA9685 共用 I2C 总线],
  [OLED], [VCC], [3V3 或兼容 VCC], [该模块标称 3.3V-5V],
  [OLED], [GND], [GND], [必须共地],
  [PCA9685], [SDA], [GPIO6], [默认地址 0x40],
  [PCA9685], [SCL], [GPIO7], [默认地址 0x40],
  [PCA9685], [VCC], [3V3], [逻辑电源],
  [PCA9685], [GND], [GND], [逻辑地与舵机地共地],
  [PCA9685], [V+], [SERVO_PW], [舵机电源，不允许接 ESP32-S3 3.3V],
  [CAN], [TX], [GPIO4], [连接到板上 SN65HVD230 的 TWAI_TX],
  [CAN], [RX], [GPIO5], [连接到板上 SN65HVD230 的 TWAI_RX],
)

#title-block(
  [auto_typer 连通性测试流程与示例代码],
  subtitle: [适用对象：ESP32-S3 主控板 + SH1106 OLED + PCA9685 舵机板 + MG996R 连续旋转舵机 + Emm_V5.0 CAN 电机],
  date: [2026-06-10],
)

#v(8pt)

#info-box([测试目标])[
  本文档用于验证整板基础连通性，覆盖以下链路：

  - 主控板上电与串口输出
  - I2C 总线连通性
  - `SH1106 128x32 OLED` 显示
  - `PCA9685` 舵机驱动板连通性
  - `1 号口 MG996R 360 度舵机` 正转、反转、停转
  - `Emm_V5.0` CAN 电机使能、速度模式、实时转速读取
]

= 测试准备

测试前请准备以下器材。

#table(
  columns: (4cm, 3cm, 8cm),
  inset: 6pt,
  fill: (x, y) => if y == 0 { rgb("#EAF1FF") } else { white },
  [项目], [数量], [要求],
  [ESP32-S3 主控板], [1], [本文档对应的 `auto_typer` 控制板],
  [SH1106 OLED], [1], [I2C 四针模块，建议默认地址 `0x3C`],
  [PCA9685 舵机驱动板], [1], [默认地址 `0x40`],
  [MG996R 舵机], [1], [按连续旋转 360 度舵机处理],
  [Emm_V5.0 CAN 电机], [1], [地址默认 `1`，总线已正确接入终端电阻],
  [USB 数据线], [1], [用于烧录和串口监视],
  [舵机电源], [1], [接到 PCA9685 的 `V+`，不要使用主控板 3.3V 直接驱动舵机],
)

= 硬件连接总览

== 已确认的主控板接口

- `CAN TX = GPIO4`
- `CAN RX = GPIO5`
- `I2C SDA = GPIO6`
- `I2C SCL = GPIO7`
- CAN 波特率按厂家例程固定为 `500 kbps`

#pin-table

== 原理图与 PCB 参考

#grid(
  columns: 2,
  gutter: 10pt,
  figure(
    image("assets/schematic_preview.png", width: 100%),
    caption: [主控板原理图预览。可见 `ESP32-S3`、`SN65HVD230`、`I2C` 引出和双路电源。],
  ),
  figure(
    image("assets/pcb_top_preview.png", width: 100%),
    caption: [主控板 PCB 预览，用于现场定位连接器与功能分区。],
  ),
)

#v(6pt)

figure(
  image("assets/pcb_detail_preview.png", width: 74%),
  caption: [PCB 细节预览。测试接线时建议结合导出 PCB PDF 一起核对接口位置。],
)

== 外设照片

#grid(
  columns: 2,
  gutter: 10pt,
  figure(
    image("assets/oled_module.png", width: 100%),
    caption: [OLED 模块。引脚顺序为 `VCC GND SCL SDA`，驱动芯片为 `SH1106`。],
  ),
  figure(
    image("assets/servo_driver_board.jpg", width: 100%),
    caption: [PCA9685 舵机板。逻辑接口为 `VCC SDA SCL OE GND`，舵机电源走右侧绿色端子。],
  ),
)

= 接线步骤

== OLED 接线

1. 将 OLED 的 `VCC` 接主控板 `3V3` 或兼容电源。
2. 将 OLED 的 `GND` 接主控板 `GND`。
3. 将 OLED 的 `SCL` 接主控板 `GPIO7`。
4. 将 OLED 的 `SDA` 接主控板 `GPIO6`。

预期结果：

- 上电后，程序应先扫描到 `0x3C` 或 `0x3D`。
- OLED 依次显示 `BOOT OK`、`I2C OK`、`OLED OK` 等状态文字。

== PCA9685 与 MG996R 接线

1. 将 `PCA9685` 的 `VCC` 接主控板 `3V3`。
2. 将 `PCA9685` 的 `GND` 接主控板 `GND`。
3. 将 `PCA9685` 的 `SCL` 接主控板 `GPIO7`。
4. 将 `PCA9685` 的 `SDA` 接主控板 `GPIO6`。
5. 将 `PCA9685` 的 `OE` 直接接 `GND`。如果 `OE` 悬空，PWM 输出可能被关闭，舵机会完全不动作。
6. 将舵机独立电源正极接 `PCA9685` 板右侧绿色端子的 `V+`。
7. 将舵机独立电源负极接 `PCA9685` 板右侧绿色端子的 `GND`。
8. 将 `MG996R` 插到 `1 号口`。

接舵机时必须确认三线方向：

- `GND` 对应黑色/棕色线
- `V+` 对应红色线
- `PWM` 对应橙色/黄色信号线

不同卖家的 `1 号口` 有时按 `CH0` 计数，有时按 `CH1` 计数。配套测试程序会同时驱动 `CH0` 和 `CH1`，避免现场因编号口径不同导致误判。

预期结果：

- 舵机先停转
- 再正转约 2 秒
- 再停转
- 再反转约 2 秒
- 再停转

若舵机在“停转”阶段仍缓慢爬行，请围绕 `1500 us` 中位脉宽微调。

== CAN 电机接线

1. 将主控板 CAN 侧接到 `Emm_V5.0` 电机驱动的 `CANH/CANL`。
2. 确认电机驱动内部已安装厂家要求的 `ZDT_CAN_V1.1` 小板。
3. 确认总线末端具备 `120Ω` 终端匹配。
4. 本文档默认电机地址为 `1`。

预期结果：

- 主控板发出使能命令
- 主控板发出 `300 RPM` 顺时针速度命令
- 主控板读取并打印实时转速
- OLED 最后一行显示 `CAN OK`

= 烧录与执行

== Arduino 环境要求

- 开发方式：`Arduino`
- 目标内核：`Arduino ESP32`
- 串口波特率：`115200`
- I2C 频率：`400 kHz`
- CAN 波特率：`500 kbps`

建议的目录为：

- 示例工程：`src/test/test/test.ino`

== 操作流程

1. 打开 Arduino IDE。
2. 安装 `ESP32` 开发板支持。
3. 选择 `ESP32-S3` 对应板型。
4. 打开本仓库中的示例工程并烧录。
5. 打开串口监视器，波特率设为 `115200`。
6. 观察 OLED、舵机与 CAN 电机动作。

推荐串口输出阶段如下：

```text
==== BOOT ====
I2C scan start
...
OLED address: 0x3C
PCA9685 detected
==== SERVO ====
Servo pulse 2000us
...
==== CAN ====
TWAI init ok
Sending velocity command: CW 300 RPM
Motor velocity response: 300.0 RPM
```

= 通过判定标准

只有满足以下全部条件时，才判定连通性测试通过：

- 串口能够稳定输出启动日志
- I2C 扫描能识别 OLED 和 PCA9685
- OLED 正常显示状态文字，无花屏、无全黑
- `1 号口 MG996R` 能完成“停转-正转-停转-反转-停转”序列
- CAN 电机能收到速度命令并返回实时转速
- 启动后 `loop()` 会轮转执行 I2C 刷新、舵机巡检和 CAN 巡检，失败设备会自动重试

= 常见故障排查

== OLED 不亮

- 检查 `VCC/GND/SCL/SDA` 顺序是否与模块一致
- 确认模块地址是 `0x3C` 或 `0x3D`
- 确认 `GPIO6/7` 未被其他跳线接错

== PCA9685 检测失败

- 检查逻辑电源 `VCC` 是否已接 `3V3`
- 检查 I2C 线是否与 OLED 共地共总线
- 确认地址焊桥未改动，默认应为 `0x40`

== 舵机不转或抖动

- 检查舵机是否插在 `1 号口`
- 检查 `GND / V+ / PWM` 方向是否反了
- 检查舵机电源是否独立供电且电流能力足够
- 连续旋转舵机若静止脉宽偏移，可微调 `1500 us`

== CAN 无反馈

- 确认 `GPIO4/5` 对应 `TWAI_TX/TWAI_RX`
- 确认总线极性 `CANH/CANL` 正确
- 确认 CAN 波特率为 `500 kbps`
- 确认电机地址为 `1`
- 确认总线末端有 `120Ω` 匹配

= 示例代码

以下代码即本文档配套的 Arduino 联通性测试程序：

#raw(read("../test/test.ino"), block: true, lang: "cpp")
