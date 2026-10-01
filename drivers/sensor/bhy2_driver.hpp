// =============================================================================
// drivers/sensor/bhy2_driver.hpp
//
// Bosch BHI260AP 智能传感器 hub 主机接口协议层 (BHY2 协议)
// 目标硬件：小米手环 8 (Xiaomi Smart Band 8) 6 轴加速度计
//
//   芯片型号    : Bosch BHI260AP (Fuser2 传感器 hub + 集成 6 轴 IMU)
//   通信接口    : I2C (从机地址 0x28 / I2C_ADDR_BHI260AP)，寄存器地址 8 位
//   数据通路    : 主机轮询 INT_STATUS → 批量读 CHAN_FIFO_W → 字节流解析
//
// 协议常量来源（已核实，勿凭记忆改动）：
//   Bosch Sensortec BHY2-Sensor-API v1.6.0 (BSD-3-Clause)
//     - bhy2_defs.h: 寄存器映射、BOOT_STATUS/INT_STATUS 位、命令包 ID
//     - bhy2.c:      系统事件大小表 bhy2_sysid_event_size、命令包 4 字节头
//
// 已知限制（fail-safe 设计前提）：
//   1. BHI260AP 是可编程 sensor hub，须有固件（flash 自举或主机上传）才产出
//      数据。本层不实现固件上传（BHY2_CMD_UPLOAD_TO_PROGRAM_RAM / BOOT_FLASH
//      为后续任务）；固件未就绪时 probe() 失败，上层 read() 返回 false。
//   2. accel passthrough 帧按 1 LSB = 1 mg 解释（计步阈值以 1g≈1000mg 为基准）。
//      实际系数取决于固件配置，待真机 HIL 标定——只影响阈值精度，不影响数据真实性。
//
// 设计原则（遵循 AGENTS.md 驱动规范，参照 gt316_driver.hpp 模式）：
//   - 零动态内存分配：固定大小静态缓冲
//   - 通过 auroraos::hal::II2cHal 抽象接口解耦硬件，主机测试注入 mock
//   - 解析器 fail-safe：截断/坏 ID/越界一律停止并上报，绝不编造数据
// =============================================================================
#ifndef AURORA_BHY2_DRIVER_HPP
#define AURORA_BHY2_DRIVER_HPP

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "../../hal/i2c_hal.hpp"

namespace auroraos {
namespace bhy2 {

// ---------------------------------------------------------------------------
// 寄存器映射（8 位地址）— bhy2_defs.h v1.6.0
// ---------------------------------------------------------------------------
constexpr uint8_t kBhy2RegChanCmd = 0x00;          // 命令通道（命令包头+载荷都写这里）
constexpr uint8_t kBhy2RegChanFifoW = 0x01;        // Wakeup FIFO 数据通道
constexpr uint8_t kBhy2RegChanFifoNw = 0x02;       // Non-wakeup FIFO 数据通道
constexpr uint8_t kBhy2RegChanStatus = 0x03;       // 状态 FIFO / 参数读回通道
constexpr uint8_t kBhy2RegProductId = 0x1C;        // 产品 ID（0x89）
constexpr uint8_t kBhy2RegBootStatus = 0x25;       // 启动状态
constexpr uint8_t kBhy2RegChipId = 0x2B;           // 芯片 ID
constexpr uint8_t kBhy2RegIntStatus = 0x2D;        // 中断状态
constexpr uint8_t kBhy2RegErrorValue = 0x2E;       // 错误值

constexpr uint8_t kBhy2ProductIdValue = 0x89;      // BHY2_PRODUCT_ID

// BOOT_STATUS 位 — bhy2_defs.h
constexpr uint8_t kBhy2BstFlashDetected = 0x01;
constexpr uint8_t kBhy2BstHostInterfaceReady = 0x10;
constexpr uint8_t kBhy2BstHostFwVerifyDone = 0x20;
constexpr uint8_t kBhy2BstHostFwVerifyError = 0x40;

// INT_STATUS 位 — bhy2_defs.h
constexpr uint8_t kBhy2IstFifoWDrdy = 0x02;        // Wakeup FIFO data ready
constexpr uint8_t kBhy2IstFifoNwDrdy = 0x08;       // Non-wakeup FIFO data ready

// 命令包 ID — bhy2_defs.h
constexpr uint16_t kBhy2CmdConfigSensor = 0x000D;  // 使能/配置虚拟传感器
constexpr uint16_t kBhy2CmdFifoFlush = 0x0009;     // 清 FIFO（流失步恢复用）

// 虚拟传感器 ID — Accelerometer passthrough（直通内部 IMU 原始加速度）
constexpr uint8_t kBhy2SensorIdAccPass = 1;

// 默认从机地址（与 board.h I2C_ADDR_BHI260AP 一致，可被板级覆盖）
#ifndef BHY2_I2C_ADDR_DEFAULT
#define BHY2_I2C_ADDR_DEFAULT 0x28
#endif
constexpr uint8_t kBhy2I2cAddrDefault = BHY2_I2C_ADDR_DEFAULT;

// 系统事件帧总大小表（含 1 字节事件 ID），下标 0 对应事件 ID 245 —
// Bosch bhy2.c bhy2_sysid_event_size（时间戳增量/meta/填充事件，解析时跳过）
constexpr uint8_t kBhy2SysEventSize[11] = {2, 3, 6, 4, 0, 18, 2, 3, 6, 4, 1};
constexpr uint8_t kBhy2SysIdBase = 245;

// ---------------------------------------------------------------------------
// BHY2 FIFO 字节流解析器
//
// BHY2 FIFO 是事件流：每帧 = 1 字节事件 ID + 载荷。
//   - ID >= 245：系统事件（时间戳增量/meta/填充），载荷大小查表，全部跳过；
//     表值 <= 1 表示仅 ID 字节本身（填充帧）。
//   - ID <  245：虚拟传感器帧。bit0 = data-less 标志（1=无载荷，仅状态通知），
//     sensor_id = ID >> 1。本层只关心 accel passthrough（载荷 6 字节：
//     3 x int16 小端 X/Y/Z）。
//   - ID == 0x00：无效帧头（sensor 0 保留），视为数据流结束标记——芯片对
//     超量读取通常回填 0x00，据此终止本块解析。
//
// 跨块帧：FIFO 按字节消费，读取分块可能在帧中间截断。解析器保留不完整尾部
// （carry buffer），下次 feed() 时先拼接再解析，保证帧边界不被块边界破坏。
// ---------------------------------------------------------------------------
class Bhy2FifoParser {
public:
    // accel passthrough 载荷：bhy2_data_xyz = 3 x int16 LE
    static constexpr size_t kAccelPayloadLen = 6;
    // 单次 feed 的最大块长（主机接口每次从 FIFO 通道读取的长度）
    static constexpr size_t kChunkLen = 64;
    // 最大帧总大小（最大系统事件 18 字节）；良好流中遗留尾部长度不会超过它
    static constexpr size_t kMaxFrameTotal = 18;

    enum class Result {
        kNoFrame,   // 本块解析干净结束，没有 accel 帧
        kGotAccel,  // 解析到至少一帧 accel（out_xyz 为最后一帧，即最新样本）
        kDesync     // 流失步（未知传感器帧/缓冲异常），调用方应清 FIFO 重对齐
    };

    Bhy2FifoParser() : buf_len_(0) {}

    void reset() {
        buf_len_ = 0;
    }

    // 输入一个 FIFO 块，输出其中最新的 accel 帧（若有）。
    // 任何结构性异常（未知传感器 ID、缓冲异常）返回 kDesync，绝不输出猜测数据。
    Result feed(const uint8_t* data, size_t len, int16_t out_xyz[3]) {
        if (!data || len == 0) {
            return Result::kNoFrame;
        }
        // 内存上限保护：遗留尾部（< kMaxFrameTotal）+ 新块不可能触发，
        // 触发即说明流已异常
        if (buf_len_ + len > sizeof(buf_)) {
            buf_len_ = 0;
            return Result::kDesync;
        }
        memcpy(buf_ + buf_len_, data, len);
        buf_len_ += len;

        Result result = Result::kNoFrame;
        size_t pos = 0;
        while (pos < buf_len_) {
            const uint8_t id = buf_[pos];

            if (id == 0x00) {
                // 数据流结束填充：丢弃本块剩余
                buf_len_ = 0;
                return result;
            }

            if (id >= kBhy2SysIdBase) {
                // 系统事件：表值为帧总大小（含 ID 字节）
                const uint8_t total = kBhy2SysEventSize[id - kBhy2SysIdBase];
                if (total <= 1) {
                    pos += 1; // 填充帧，仅 ID 字节
                    continue;
                }
                if (buf_len_ - pos < total) {
                    break; // 帧被块边界截断，留待下次
                }
                pos += total;
                continue;
            }

            // 虚拟传感器帧
            if (id & 0x01) {
                pos += 1; // data-less 通知，无载荷
                continue;
            }
            const uint8_t sensor_id = static_cast<uint8_t>(id >> 1);
            if (sensor_id != kBhy2SensorIdAccPass) {
                // 只使能了 accel passthrough，出现其它传感器帧说明流已失步
                buf_len_ = 0;
                return Result::kDesync;
            }
            const size_t frame_len = 1 + kAccelPayloadLen;
            if (buf_len_ - pos < frame_len) {
                break; // 截断，留待下次
            }
            // int16 小端解包（最后一帧胜出 = 最新样本）
            const uint8_t* p = &buf_[pos + 1];
            out_xyz[0] = static_cast<int16_t>(static_cast<uint16_t>(p[0]) |
                                              (static_cast<uint16_t>(p[1]) << 8));
            out_xyz[1] = static_cast<int16_t>(static_cast<uint16_t>(p[2]) |
                                              (static_cast<uint16_t>(p[3]) << 8));
            out_xyz[2] = static_cast<int16_t>(static_cast<uint16_t>(p[4]) |
                                              (static_cast<uint16_t>(p[5]) << 8));
            result = Result::kGotAccel;
            pos += frame_len;
        }

        // 压缩：把未消费的尾部（不完整帧）挪到缓冲头部
        if (pos > 0) {
            memmove(buf_, buf_ + pos, buf_len_ - pos);
            buf_len_ -= pos;
        }
        return result;
    }

private:
    // 工作缓冲 = 遗留尾部 + 新块。良好流中遗留尾部 < kMaxFrameTotal，
    // 因此 kMaxFrameTotal + kChunkLen 之外的空间永远不会用到，溢出即失步。
    static constexpr size_t kWorkBufLen = kMaxFrameTotal + kChunkLen + 32;
    uint8_t buf_[kWorkBufLen];
    size_t buf_len_;
};

// ---------------------------------------------------------------------------
// BHY2 主机接口：探测 / 使能 accel / 读 FIFO
// ---------------------------------------------------------------------------
class Bhy2HostInterface {
public:
    Bhy2HostInterface() : i2c_(nullptr), addr_(kBhy2I2cAddrDefault) {}

    void configure(auroraos::hal::II2cHal* i2c, uint8_t dev_addr = kBhy2I2cAddrDefault) {
        i2c_ = i2c;
        addr_ = dev_addr;
        parser_.reset();
    }

    bool is_configured() const {
        return i2c_ != nullptr;
    }

    // 探测芯片在位且固件就绪。
    //  - CHIP_ID 读到 0x00/0xFF 视为总线无应答/无器件（在位判断，具体家族值
    //    待 HIL 记录，不做硬编码白名单）
    //  - BOOT_STATUS 必须置位 HOST_INTERFACE_READY；FW_VERIFY_ERROR 置位视为
    //    固件校验失败（hub 无固件时通常不会置位 READY）
    bool probe() {
        if (!i2c_) {
            return false;
        }
        uint8_t chip_id = 0;
        if (!read_reg(kBhy2RegChipId, &chip_id, 1)) {
            return false;
        }
        if (chip_id == 0x00 || chip_id == 0xFF) {
            return false;
        }
        uint8_t boot_status = 0;
        if (!read_reg(kBhy2RegBootStatus, &boot_status, 1)) {
            return false;
        }
        if (boot_status & kBhy2BstHostFwVerifyError) {
            return false;
        }
        return (boot_status & kBhy2BstHostInterfaceReady) != 0;
    }

    // 使能 accel passthrough 虚拟传感器。
    // 命令包（bhy2_hif_exec_sensor_conf_cmd 语义）：
    //   头 4 字节 [cmd_lo cmd_hi payload_len_lo payload_len_hi] 写 CHAN_CMD，
    //   随后 9 字节载荷写 CHAN_CMD：
    //   [sensor_id u8][sample_rate f32 LE][latency u32 LE]
    bool enable_accel(float sample_rate_hz, uint32_t latency_ms) {
        if (!i2c_) {
            return false;
        }
        uint8_t payload[9];
        payload[0] = kBhy2SensorIdAccPass;
        memcpy(&payload[1], &sample_rate_hz, 4); // IEEE754 小端（ARM/RISC-V/x86 均 LE）
        memcpy(&payload[5], &latency_ms, 4);
        if (!exec_cmd(kBhy2CmdConfigSensor, payload, sizeof(payload))) {
            return false;
        }
        parser_.reset(); // 重新使能后 FIFO 语义重置
        return true;
    }

    // 读取最新 accel 样本。无新数据 / I2C 失败 / 解析失败一律返回 false。
    // out_xyz 单位解释见文件头"已知限制"第 2 条。
    bool read_accel(int16_t out_xyz[3]) {
        if (!i2c_ || !out_xyz) {
            return false;
        }
        uint8_t int_status = 0;
        if (!read_reg(kBhy2RegIntStatus, &int_status, 1)) {
            return false;
        }
        if ((int_status & kBhy2IstFifoWDrdy) == 0) {
            return false; // 本周期无新数据，属正常轮询空转
        }

        uint8_t chunk[Bhy2FifoParser::kChunkLen];
        if (!read_reg(kBhy2RegChanFifoW, chunk, sizeof(chunk))) {
            return false;
        }

        switch (parser_.feed(chunk, sizeof(chunk), out_xyz)) {
        case Bhy2FifoParser::Result::kGotAccel:
            return true;
        case Bhy2FifoParser::Result::kNoFrame:
            return false;
        case Bhy2FifoParser::Result::kDesync:
        default: {
            // 流失步恢复：清 FIFO 重新对齐（payload 0x03 = 唤醒 + 非唤醒通道，
            // 位语义待 HIL 确认；失败也不影响下次轮询）
            uint8_t flush_all = 0x03;
            exec_cmd(kBhy2CmdFifoFlush, &flush_all, 1);
            return false;
        }
        }
    }

private:
    bool read_reg(uint8_t reg, uint8_t* data, size_t len) {
        return i2c_->read_reg(addr_, reg, data, len);
    }

    bool write_reg(uint8_t reg, const uint8_t* data, size_t len) {
        return i2c_->write_reg(addr_, reg, data, len);
    }

    // BHY2 命令包：头与载荷分两次写 CHAN_CMD（bhy2_hif_exec_cmd 语义）
    bool exec_cmd(uint16_t cmd, const uint8_t* payload, uint8_t payload_len) {
        const uint8_t header[4] = {
            static_cast<uint8_t>(cmd & 0xFF),
            static_cast<uint8_t>((cmd >> 8) & 0xFF),
            payload_len,
            0
        };
        if (!write_reg(kBhy2RegChanCmd, header, sizeof(header))) {
            return false;
        }
        if (payload_len != 0 && payload != nullptr) {
            return write_reg(kBhy2RegChanCmd, payload, payload_len);
        }
        return true;
    }

    auroraos::hal::II2cHal* i2c_;
    uint8_t addr_;
    Bhy2FifoParser parser_;
};

} // namespace bhy2
} // namespace auroraos

#endif // AURORA_BHY2_DRIVER_HPP
