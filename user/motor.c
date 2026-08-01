/**
  * @file    motor.c
  * @brief   张大头闭环步进电机底层协议驱动
  * @author  AI Agent
  * @note    依赖 huart3（已由 CubeMX 初始化）
  *          使用 HAL_UART_Transmit/Receive 阻塞模式，不依赖中断接收
  */
#include "motor.h"
#include "usart.h"
#include <math.h>       /* fabsf */
#include <string.h>     /* memset */

/* ======================== 内部变量 ======================== */

static float motor_current[5];  /* 各轴当前目标位置，索引 1~4，地址 0 不用 */

/* ======================== 内部辅助函数 ======================== */

/**
  * @brief  获取指定电机的正方向
  * @param  addr  电机地址
  * @retval MOTOR_DIR_CW 或 MOTOR_DIR_CCW
  */
static uint8_t Motor_GetForwardDir(uint8_t addr)
{
    switch (addr) {
        case MOTOR_ADDR_X:   return MOTOR_X_DIR_FORWARD;
        case MOTOR_ADDR_Y:   return MOTOR_Y_DIR_FORWARD;
        case MOTOR_ADDR_Z:   return MOTOR_Z_DIR_FORWARD;
        case MOTOR_ADDR_YAW: return MOTOR_YAW_DIR_FORWARD;
        default:             return MOTOR_DIR_CW;
    }
}

/**
  * @brief  获取指定电机的转速
  * @param  addr  电机地址
  * @retval RPM 值
  */
static uint16_t Motor_GetRPM(uint8_t addr)
{
    switch (addr) {
        case MOTOR_ADDR_X:   return MOTOR_X_RPM;
        case MOTOR_ADDR_Y:   return MOTOR_Y_RPM;
        case MOTOR_ADDR_Z:   return MOTOR_Z_RPM;
        case MOTOR_ADDR_YAW: return MOTOR_YAW_RPM;
        default:             return 500U;
    }
}

/**
  * @brief  取反方向
  * @param  dir  当前方向
  * @retval 相反方向
  */
static uint8_t Motor_ReverseDir(uint8_t dir)
{
    return (dir == MOTOR_DIR_CW) ? MOTOR_DIR_CCW : MOTOR_DIR_CW;
}

/**
  * @brief  清除 UART3 接收缓冲中残留的脏数据
  * @note   在发送新命令前调用，防止上次超时遗留数据干扰
  *         先中止接收 → 清错误标志 → 有限读空 → 再中止，确保干净启动
  */
static void Motor_FlushRx(void)
{
    uint8_t dummy;
    uint8_t i;

    /* 中止任何正在进行的接收，重置 HAL 状态为 READY */
    HAL_UART_AbortReceive(&huart3);

    /* 清除 UART 错误标志（浮空引脚噪声会触发 ORE/NE/FE） */
    __HAL_UART_CLEAR_OREFLAG(&huart3);
    __HAL_UART_CLEAR_NEFLAG(&huart3);
    __HAL_UART_CLEAR_FEFLAG(&huart3);

    /* 排空残留数据，最多 32 字节，防止死循环 */
    for (i = 0; i < 32; i++) {
        if (HAL_UART_Receive(&huart3, &dummy, 1, 2) != HAL_OK) {
            break;
        }
    }

    /* 再次中止，确保状态干净 */
    HAL_UART_AbortReceive(&huart3);
    __HAL_UART_CLEAR_OREFLAG(&huart3);
    __HAL_UART_CLEAR_NEFLAG(&huart3);
    __HAL_UART_CLEAR_FEFLAG(&huart3);
}

/**
  * @brief  通过 UART3 发送帧
  * @param  data  帧数据
  * @param  len   帧长度
  * @retval 0=成功, -1=发送失败
  */
static int Motor_SendFrame(const uint8_t *data, uint8_t len)
{
    if (HAL_UART_Transmit(&huart3, (uint8_t *)data, len, 100) != HAL_OK) {
        return -1;
    }
    /* 帧间间隔 ≥10ms */
    HAL_Delay(MOTOR_FRAME_INTERVAL_MS);
    return 0;
}

/**
  * @brief  等待绝对定位到位应答 ({addr} FD 9F 6B)
  * @param  addr       电机地址
  * @param  timeout_ms 超时时间
  * @retval 0=到位, -1=超时/错误
  */
static int Motor_WaitArrived(uint8_t addr, uint32_t timeout_ms)
{
    uint8_t resp[4];

    if (HAL_UART_Receive(&huart3, resp, 4, timeout_ms) != HAL_OK) {
        return -1;
    }
    if (resp[0] != addr || resp[1] != 0xFD || resp[2] != 0x9F || resp[3] != 0x6B) {
        return -1;
    }
    return 0;
}

/* ======================== 指令发送函数 ======================== */

int Motor_Enable(uint8_t addr)
{
    uint8_t frame[4];

    frame[0] = addr;
    frame[1] = 0xF3;
    frame[2] = 0x00;
    frame[3] = 0x6B;

    /* 只管发送，无需等待回发 */
    return Motor_SendFrame(frame, sizeof(frame));
}

int Motor_Zero(uint8_t addr)
{
    uint8_t frame[4];

    frame[0] = addr;
    frame[1] = 0x0A;
    frame[2] = 0x6D;
    frame[3] = 0x6B;

    /* 复位当前位置追踪 */
    motor_current[addr] = 0.0f;

    /* 只管发送，无需等待回发 */
    return Motor_SendFrame(frame, sizeof(frame));
}

int Motor_Stop(uint8_t addr)
{
    uint8_t frame[3];

    frame[0] = addr;
    frame[1] = 0xF7;
    frame[2] = 0x6B;

    return Motor_SendFrame(frame, sizeof(frame));
}

int Motor_SendMoveTo(uint8_t addr, float value, MotorUnit unit)
{
    uint8_t frame[13];
    float pulse_f;
    uint32_t pulse_abs;
    uint8_t dir;
    uint16_t rpm;

    /* 已在目标位置 → 跳过，无需发送 */
    if (fabsf(value - motor_current[addr]) < 0.01f) {
        return 1;
    }

    /* ---- 计算脉冲值和方向 ---- */
    if (unit == UNIT_MM) {
        pulse_f = value * 1600.0f;
    } else {
        pulse_f = fabsf(value) * 6400.0f / 360.0f;
    }

    dir = (value >= 0.0f) ? Motor_GetForwardDir(addr) : Motor_ReverseDir(Motor_GetForwardDir(addr));
    pulse_abs = (uint32_t)fabsf(pulse_f);
    rpm = Motor_GetRPM(addr);

    /* ---- 组帧 ---- */
    frame[0]  = addr;
    frame[1]  = 0xFD;
    frame[2]  = dir;
    frame[3]  = (uint8_t)(rpm >> 8);
    frame[4]  = (uint8_t)(rpm & 0xFF);
    frame[5]  = MOTOR_ACCEL;
    frame[6]  = (uint8_t)(pulse_abs >> 24);
    frame[7]  = (uint8_t)(pulse_abs >> 16);
    frame[8]  = (uint8_t)(pulse_abs >> 8);
    frame[9]  = (uint8_t)(pulse_abs & 0xFF);
    frame[10] = 0x01;
    frame[11] = 0x00;
    frame[12] = 0x6B;

    /* ---- 发送（不等待应答） ---- */
    if (Motor_SendFrame(frame, sizeof(frame)) == 0) {
        motor_current[addr] = value;  /* 发送成功，更新追踪 */
        return 0;
    }
    return -1;
}

int Motor_WaitMoveDone(uint8_t addr)
{
    return Motor_WaitArrived(addr, MOTOR_TIMEOUT_MS);
}

int Motor_WaitAllDone(const uint8_t *addrs, uint8_t count, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    uint8_t pending = 0;
    uint8_t i;

    /* 初始化：全部标记为待处理 */
    for (i = 0; i < count; i++) {
        pending |= (1 << i);
    }

    while (pending) {
        uint8_t resp[4];

        /* 短超时读取，收满 4 字节即返回 */
        if (HAL_UART_Receive(&huart3, resp, 4, 20) == HAL_OK) {
            /* 检查应答是否属于某个待处理的轴 */
            for (i = 0; i < count; i++) {
                if ((pending & (1 << i))
                    && resp[0] == addrs[i]
                    && resp[1] == 0xFD
                    && resp[2] == 0x9F
                    && resp[3] == 0x6B) {
                    pending &= ~(1 << i);   /* 标记该轴到位 */
                    break;
                }
            }
        }

        /* 总超时检查 */
        if (HAL_GetTick() - start > timeout_ms) {
            return -1;
        }
    }
    return 0;
}

void Motor_Init(void)
{
    uint8_t addrs[] = {MOTOR_ADDR_X, MOTOR_ADDR_Y, MOTOR_ADDR_Z, MOTOR_ADDR_YAW};
    uint8_t i;

    HAL_Delay(1000);

    /* 初始化位置追踪 */
    for (i = 1; i <= 4; i++) {
        motor_current[i] = 0.0f;
    }

    /* 先逐个使能（无回发等待，仅 10ms 帧间隔） */
    for (i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
        Motor_Enable(addrs[i]);
    }

    /* 再逐个清零 */
    for (i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
        Motor_Zero(addrs[i]);
    }
}
