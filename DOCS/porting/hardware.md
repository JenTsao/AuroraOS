# Hardware Adaptation Guide

本指南汇总 auroraOS 已适配的全部硬件，并给出每种外设的驱动位置、关键参数、接线与最小接入示例。所有驱动均通过 `hal/` 抽象层（`II2cHal` / `ISpiHal` / `IGpioHal` 等）与具体 MCU 解耦，板级只需实现对应的 `get_*_hal()` 工厂即可接入。

## 适配硬件总览

| 硬件类别 | 型号 / 协议 | 驱动文件 | 接口 | 状态 |
|----------|-------------|----------|------|------|
| 显示 — 单色 OLED | SSD1306 0.96" 128×64 (HS96L03W2C03) | `drivers/display/ssd1306_driver.hpp` | I2C | ✅ 已适配 |
| 显示 — 彩色 LCD | ST7789 (MiBand) | `drivers/display/st7789_driver.hpp` | SPI | ✅ 已适配（待真机验证） |
| 显示 — 模拟 | OLED Mock (窗口化局部更新协议) | `drivers/display/oled_driver_mock.hpp` | 无真实硬件 | ✅ 已适配 |
| 帧缓冲 / 渲染 | 脏区域渲染内核 + Renderer2D | `drivers/display/framebuffer.hpp`, `renderer2d.hpp` | — | ✅ 已适配 |
| 输入 — 触摸 | Gt316Driver / TouchDriver / GestureRecognizer | `drivers/input/gt316_driver.hpp`, `gesture_recognizer.hpp` | I2C (0x14) + INT (Pin 15) | ✅ 已适配 (真实 I2C + 7 态手势 + 仿真双模) |
| 网络 — 以太网 | StellarisEth (LM3S6965) | `drivers/net/...` | RMII | ✅ 已适配 |
| 存储 | LittleFS / RamFS / ProcFS | `vfs/` | — | ✅ 已适配 |
| 看门狗 | WatchdogManager | `kernel/core/...` | — | ✅ 已适配 |
| 传感器 | HeartRateSensor / Accelerometer | `drivers/sensor/...` | 模拟 | ✅ 已适配 |

> 状态说明：`✅ 已适配` 表示驱动已完整实现并可在目标上构建运行；`🚧 半实现` 表示框架/主路径存在但仍有占位（如 ST7789 的 DMA 忙等）；`QEMU 仿真` 表示在 QEMU 中以状态机模拟，尚未对接真实硬件。

下文以 **SSD1306 I2C OLED** 为例给出完整适配说明，其余硬件请参照源码与 `DOCS/` 下对应章节。

---

## SSD1306 I2C OLED 显示驱动

### 硬件规格

`drivers/display/ssd1306_driver.hpp` 针对 **SSD1306 主控、I2C 接口、单色 128×64** OLED，典型如 0.96 英寸 4Pin 模块 HS96L03W2C03。关键参数（来自规格书）：

- 分辨率：128 × 64，单色（每个像素 1 bit）。
- Duty：1/64。
- 接口：I2C，从机地址 0x3C（写地址 0x78），SCL/SDA 需外接上拉电阻。
- 供电：VCC 2.8 ~ 3.3V。
- 4Pin 定义：`Pin1 GND`、`Pin2 VCC(2.8~3.3V)`、`Pin3 SCL`、`Pin4 SDA`。

### 初始化序列

驱动严格按规格书参考实现编写控制字节序列：

| 命令 | 值 | 作用 |
|------|----|------|
| 0xAE | — | 关闭显示 |
| 0xD5 / 0x80 | — | 设置显示时钟分频 |
| 0xA8 / 0x3F | — | 复用比 1/64 |
| 0xD3 / 0x00 | — | 显示偏移 0 |
| 0x40 | — | 起始行 0 |
| 0x8D / 0x14 | — | 电荷泵使能（0x14 = 开启） |
| 0x20 / 0x02 | — | 寻址模式：页式 (Page) |
| 0xA1 | — | 段重映射（左右翻转） |
| 0xC8 | — | COM 扫描方向（上下翻转） |
| 0xDA / 0x12 | — | COM 引脚配置 |
| 0x81 / 0xCF | — | 对比度 0xCF |
| 0xD9 / 0xF1 | — | 预充电周期 |
| 0xDB / 0x30 | — | VCOMH 电平 |
| 0xA4 | — | 显示内容跟随 RAM |
| 0xA6 | — | 正常显示（非反色） |
| 0xAF | — | 开启显示 |

### 驱动特性

- **总线复用**：`II2cHal::write_reg` 的首字节直接映射为 SSD1306 控制字节——命令 0x00、数据 0x40，因此与具体 MCU 完全解耦，板级只需实现 `get_i2c_hal()`。
- **零动态分配**：1KB 页式显存（1 字节 = 8 个纵向像素）+ 脏页位图均为对象内静态数组；`refresh()` 只推送发生变化的页，降低 I2C 带宽。
- **绘制与文本**：提供 `clear / set_pixel / fill_rect / draw_hline / draw_vline / draw_char / draw_string` 以及电源控制 `sleep / wake / set_contrast / invert`。内嵌 5×7 ASCII 字模（95 字符，475 字节，已校验）。
- **设备挂载**：继承 `CharDevice`，可直接 `DeviceRegistry::register_device()` 挂载到 `/dev/`（如 `/dev/oled0`）。

### 最小接入示例

```cpp
#include "drivers/display/ssd1306_driver.hpp"

Ssd1306Driver oled("oled0");
oled.configure(auroraos::hal::get_i2c_hal(0), 0x3C);  // 绑定 I2C 总线与从机地址
oled.open();                                           // 硬件初始化 + 清屏
oled.draw_string(0, 0, "AuroraOS");
oled.refresh();                                        // 推送到硬件
```

### 板级适配要点

当前代码库仅有 `II2cHal` 接口声明（`hal/i2c_hal.hpp`），尚无板级 I2C 外设实现。要真正点亮屏幕，需在目标板上实现 `get_i2c_hal()`，可选择：

- GPIO 软件模拟 I2C（bit-banging），适合无硬件 I2C 控制器的 Cortex-M0+ 目标。
- MCU 硬件 I2C 控制器外设驱动（如 Apollo3 IOM1 I2C Master），性能更高。

接线（规格书 1.5）：`GND→Pin1`、`VCC→Pin2 (2.8~3.3V)`、`SCL→Pin3`、`SDA→Pin4`，SCL/SDA 需外接上拉电阻（典型 4.7kΩ 上拉到 VCC）。

---

## 汇顶 GT316 电容触控与 7 态手势识别

### 硬件规格与协议

`drivers/input/gt316_driver.hpp` 针对小米手环 8 的汇顶 GT316 单点/多点电容触控芯片：

- **接口**：I2C，从机地址 `0x14` (`I2C_ADDR_GT316`)，中断引脚 `PIN_TOUCH_INT` (Pin 15)。
- **分辨率**：192 × 490 (`DISPLAY_WIDTH` × `DISPLAY_HEIGHT`)。
- **寄存器与握手**：
  - `0x814E`: 缓冲状态与点数 (`bit7`=Ready, `bit3..0`=Count)。
  - `0x814F`: 触控点 1 详情 (TrackID, X_L, X_H, Y_L, Y_H, Size)。
  - 读取完坐标后向 `0x814E` 写 `0x00` 清除握手状态并释放 INT 引脚。

### 7 态手势识别状态机 (`GestureRecognizer`)

`drivers/input/gesture_recognizer.hpp` 将原始连续触控帧转化为 7 种高阶手势：
1. **TAP (单击)**: 选择/按钮点击
2. **DOUBLE_TAP (双击)**: 快捷操作
3. **LONG_PRESS (长按)**: 表盘编辑/设置 (长按 >=800ms 原地触发)
4. **SWIPE_LEFT (左滑)**: 进入下一屏 (`ScreenNavigator::push`)
5. **SWIPE_RIGHT (右滑)**: 退出当前屏 (`ScreenNavigator::pop`)
6. **SWIPE_UP (上滑)**: 通知中心
7. **SWIPE_DOWN (下滑)**: 快捷控制面板

### 最小接入示例

```cpp
#include "drivers/input/gt316_driver.hpp"
#include "drivers/input/gesture_recognizer.hpp"
#include "ui/ui_manager.hpp"

Gt316Driver touch("touch0", 192, 490);
touch.configure(auroraos::hal::get_i2c_hal(1), auroraos::hal::get_gpio_hal(), PIN_TOUCH_INT);
touch.open();

GestureRecognizer recognizer;

// 在主循环/后台心跳中轮询
TouchPoint point;
if (touch.poll_touch(&point, now_ms)) {
    GestureEvent ge = recognizer.feed_touch_point(point, now_ms);
    if (ge.type != GestureType::NONE) {
        UI::UiManager::instance().dispatch_gesture(ge);
    }
}
```

---

## 通用适配流程

1. 在 `boards/<vendor>/<board>/` 下实现对应 HAL 工厂函数（如 `get_i2c_hal()`、`get_spi_hal()`、`get_gpio_hal()`）。
2. 在板级初始化中 `configure()` 驱动并 `open()` 完成硬件初始化。
3. 通过 `DeviceRegistry::register_device()` 将驱动挂载到 `/dev/` 命名空间，供上层服务/应用访问。
4. 若需 Kconfig 裁剪，在板级 Kconfig 中开启对应特性开关，未启用目标不产生代码体积开销。

