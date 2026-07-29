# Claude.md — AI 编码助手项目指导

## 项目概览

| 属性 | 值 |
|---|---|
| **芯片** | STM32F103C8T6（Cortex-M3, 64KB Flash, 20KB SRAM） |
| **工具链** | Keil MDK-ARM V5.32 / ARMCLANG V6.24 |
| **HAL 框架** | STM32Cube FW_F1 V1.8.7 + CubeMX 6.17.0 |
| **项目状态** | 业务逻辑开发阶段 |

---

## 构建与开发

- **IDE 构建**：通过 Keil MDK 打开 `MDK-ARM/diansai.uvprojx` → F7 编译 → F8 下载
- **无 CLI 构建**：本项目没有 Makefile / CMakeLists，`run_in_terminal` 执行编译命令**无效**
- **烧录**：编译产物 `MDK-ARM/diansai/diansai.hex`，通过 DAP-Link 工具烧录
- **调试**：仅保留 SWD（PA13/PA14），JTAG 已在 `HAL_MspInit()` 中禁用

### 新增文件到 Keil 工程

在 `user/` 目录下新建的 `.c/.h` 文件必须手动添加到 Keil 工程树中：

1. 在 Keil IDE 左侧 Project 窗口点击 `diansai` 分组右键 → **Add Existing Files to Group 'diansai'…**
2. 选择 `user/motor.c`，加入工程
3. 点击魔术棒 → **C/C++ (AC6)** → **Include Paths** → 添加 `../user/` 路径
4. 点 OK 保存设置

> 不执行第 3 步会导致头文件 `motor.h` 找不到。

---

## 用户模块

`user/` 目录存放非 CubeMX 生成的业务代码。

| 文件 | 说明 |
|------|------|
| `motor.h` | 步进电机驱动 — 地址宏、方向速度配置、`Motor_MoveTo` API |
| `motor.c` | 帧组装（float 精算→int32 组帧）、阻塞等待到位、超时重试 |

### 步进电机驱动 API

```c
void Motor_Init(void);                                           // 上电使能+清零四轴
int  Motor_SendMoveTo(uint8_t addr, float value, MotorUnit unit); // 发绝对定位指令，不等待
int  Motor_WaitMoveDone(uint8_t addr);                            // 等待指定电机到位，±20s 超时
int  Motor_Enable(uint8_t addr);                                 // 使能（发后即回，不等应答）
int  Motor_Zero(uint8_t addr);                                   // 清零（发后即回，不等应答）
int  Motor_Stop(uint8_t addr);                                   // 急停（发后即回，不等应答）
```

- X/Y/Z 传 `mm`，Yaw 传 `°`，单位由 `MotorUnit` 枚举指定
- 全程 `float` 精确计算，仅在发帧前转 `int32_t`
- `Motor_SendMoveTo` + `Motor_WaitMoveDone` 两步分离，支持多轴同时运动
- 到位应答 `{addr} FD 9F 6B`，超时 20s 后重发一次

### 坐标映射

机器上电归零位（电机零点）与上位机坐标系原点存在固定偏移，Z/Yaw 无偏移。

```c
// motor.h — 待实测后填入实际值
#define HOME_OFFSET_X_MM    0.0f
#define HOME_OFFSET_Y_MM    0.0f
```

**映射公式**（调用 `Motor_SendMoveTo` 时）：

```
电机 X 目标 = 上位机 X + HOME_OFFSET_X_MM
电机 Y 目标 = 上位机 Y + HOME_OFFSET_Y_MM
电机 Z/Yaw 目标 = 上位机值（透传）
```

### Z 轴升降位置

Z 轴只有抬起/放下两种状态，直接使用宏定义：

```c
#define Z_HEIGHT_RAISE_MM   0.0f   /* 抬起高度（距离零点） */
#define Z_HEIGHT_LOWER_MM   15.0f  /* 放下高度（距离零点） */
```

---

## 代码生成规则（必须遵守）

所有 CubeMX 自动生成的文件（`main.c`、`gpio.c`、`tim.c`、`usart.c`、`stm32f1xx_it.c` 等）都有如下标记：

```c
/* USER CODE BEGIN <Section> */
// 你的代码写在这里
/* USER CODE END <Section> */
```

- **只能在 `USER CODE BEGIN/END` 之间写代码**
- 在标记外修改会被 CubeMX 重新生成时覆盖
- 新增用户模块请创建 `user/` 目录，不要混入 `Core/Src/` 和 `Core/Inc/`

---

## 项目功能

### 系统角色

下位机控制板，接收上位机串口指令，驱动 4 轴运动平台完成小铁片的抓取-放置（pick-and-place）操作。

### 机械结构

4 轴运动平台：

| 轴名 | 功能 | 电机地址 |
|:---:|---|:---:|
| **X** | 水平移动 | 1 |
| **Y** | 水平移动 | 2 |
| **Z** | 升降（电磁铁装在末端） | 3 |
| **Yaw** | 旋转轴 | 4 |

### 操作流程（一问一答交互模式）

```
上位机 → UART1 发送指令（目标坐标）
       ↓
下位机解析指令 → UART3 驱动步进电机移动到目标位置
       ↓
到位后 → UART1 回报"到位确认"
       ↓
上位机 → 发送下一条指令
       ↓
...（重复直至所有任务完成）
       ↓
全部完成后 → LED 闪烁（0.5Hz）+ 蜂鸣器间歇鸣叫（0.5s 响 / 0.5s 停），持续 5 秒
```

### 典型 pick-and-place 动作序列

1. X/Y 轴移动到铁片上方
2. Z 轴下降 → 电磁铁通电吸合（PB4 = 低电平）
3. Z 轴上升
4. X/Y/旋转轴移动到目标位置上方
5. Z 轴下降 → 电磁铁释放（PB4 = 高电平）
6. Z 轴上升回到安全高度

### 安全机制

- 无硬件限位开关，采用**软件限位**（在程序中设定各轴行程范围）
- 电机驱动器的堵转保护（闭环步进电机自带）

---

## 当前外设配置

| 外设 | 引脚 | 功能 | 关键参数 |
|---|---|---|---|
| **GPIO** | PC13 | 板载 LED（高电平点亮） | Output PP, Pull-up |
| **GPIO** | PA3 | 有源蜂鸣器（高电平响） | Output PP，初始低电平 |
| **GPIO** | PB4 | 电磁铁控制（低电平吸合） | Output PP，初始输出高（释放） |
| **USART1** | PA9/PA10 | 上位机通信 | 115200-8N1, 中断接收，一问一答 |
| **USART3** | PB10/PB11 | 步进电机通信（张大头协议） | 115200-8N1, 地址 1/2/3/4 |
| **SWD** | PA13/PA14 | 调试接口 | JTAG 已禁用 |


---

## 文件级编译优化覆盖

全局为 `-O`

---

## 硬件约束

- **PC13 驱动电流 ≤ 3mA**（LQFP48 封装限制），不能直接驱动大电流 LED
- **HSE 晶振 8MHz 必需存在**，未使能 CSS（时钟安全系统），晶振失效不会自动切 HSI
- **堆栈**：Stack 1KB / Heap 512B，printf / 大数组 / 递归可能栈溢出
- **PA3**（有源蜂鸣器）高电平响、低电平停，通过 GPIO 输出控制，非 PWM

---

## 通信协议

### UART1 — 上位机 ↔ 下位机

- **波特率**：115200-8N1
- **模式**：一问一答，下位机不主动发送
- **指令格式**：待定义（由你自行设计）
- **回报内容**：到位确认 + 任务完成信号

### UART3 — 张大头闭环步进电机

- **波特率**：115200-8N1，HEX 格式
- **总线**：单路 UART，4 个电机共享，通过地址（1~4）区分，`00H` 为广播地址
- **帧间间隔**：≥10ms
- **协议详情**：见桌面文件 `张大头步进电机协议总结.md`

#### 帧结构

```
┌──────┬──────┬───────────┬──────┐
│ addr │ func │   data    │  CS  │
│ 1B   │ 1B   │  N bytes  │  1B  │
└──────┴──────┴───────────┴──────┘
```

| 字段 | 说明 |
|------|------|
| addr | 电机地址 (01H~20H) |
| func | 功能码 |
| data | 参数，多字节大端序 (MSB first) |
| CS | 固定 `0x6B`，帧尾标识（非校验，无检错能力） |

#### 常用指令

| 指令 | func | 说明 |
|------|:---:|------|
| 使能 | `F3` | **上电后必须先发**，电机才响应运动指令 |
| 停止 | `F7` | 立即停止当前运动 |
| 清零 | `0A` | 当前位置设为绝对零点 |
| 绝对定位 | `FD` | 核心运动指令（13 字节），见下 |
| 读状态 | `3A` | 查询编码器位置、电流等 |

#### 绝对定位帧 (0xFD) — 13 字节

```
addr FD dir speed_H speed_L accel pulse[4B BE] mode sync CS
 1   2   3      4       5      6      7-10      11   12  13
```

| 字段 | 位置 | 说明 |
|------|:---:|------|
| dir | 3 | `00`=顺时针, `01`=逆时针 |
| speed | 4-5 | 匀速转速 RPM，大端无符号 |
| accel | 6 | 加速度系数，默认 `64H`(100) |
| pulse | 7-10 | 目标脉冲数，**大端无符号 int32** |
| mode | 11 | `01`=绝对定位 |
| sync | 12 | `00`=立即执行 |

#### 脉冲换算（⚠️ 精度敏感）

```
步距角 1.8°, 32 细分, 4mm 导程
每圈脉冲数 = 360° ÷ 1.8° × 32 = 6400 ppr
```

$$pulse = \text{distance}_{\text{mm}} \times \frac{6400}{4} = \text{distance}_{\text{mm}} \times 1600$$

**编码规范**：

- ✅ 全程用 `float` 浮点精确计算：`pulse_f = user_mm * 1600.0f`
- ✅ **仅在串口发送前**转为 `int32_t` 整型填入帧
- ❌ 禁止用近似公式（如 `×18` 或 `×1620`），累积误差不可接受

> 示例：100.0mm → `pulse_f = 160000.0f` → 发送 `int32_t pulse = 160000`

#### 通信要点

- 回应格式：`addr + func回显 + 状态码 + 6B`，`02`=成功/到位
- **上电先发 F3 使能各轴** → 再发 0A 清零回零 → 之后才能发 FD 绝对定位
- **无校验机制**：CS 固定值不作校验和，建议代码层加超时重发（如 500ms 无回应重发一次）
- **到位应答**：发送 FD 后，电机到位时回发 `{addr} FD 9F 6B`（如 `01 FD 9F 6B`），**收到此应答后才能判定移动完成**，不可仅靠延时等待
- **使能/清零/停止**：上位机发帧即回，无需等待电机应答
- 绝对定位到位后电机会自锁，编码器持续监测纠偏

---

## 🚨 待处理问题

### 1. 完成信号实现注意事项（🟢 低）

- LED（PC13, 高电平点亮）：0.5Hz 闪烁 = 亮 0.5s / 灭 0.5s，非阻塞方式
- 蜂鸣器（PA3, 有源）：间歇响 = 高电平 0.5s / 低电平 0.5s，HAL_GPIO_WritePin 控制

---

## 编码惯例

- **变量命名**：外设句柄用 CubeMX 默认的 `huart1`、`htim2` 风格
- **函数命名**：初始化函数 `MX_<PERIPH>_Init()`，回调函数 `HAL_<PERIPH>_<Callback>()`
- **头文件引入链**：`main.h` → `stm32f1xx_hal.h`，所有模块头文件 include `main.h`
- **错误处理**：初始化失败调用 `Error_Handler()`（目前仅关中断+死循环，无调试输出）
- **预处理器宏**：`USE_HAL_DRIVER, STM32F103xB`

---

## 启用/禁用 HAL 模块

当前启用：`GPIO` `TIM` `UART` `CORTEX` `DMA` `FLASH` `EXTI` `PWR` `RCC`

如需新增外设（ADC、SPI、I2C 等），优先用 CubeMX 图形界面修改 `.ioc`，再重新生成代码（确保用户代码在 `USER CODE` 标记内）。
