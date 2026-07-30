/**
  * @file    motor.h
  * @brief   张大头闭环步进电机底层协议驱动
  * @author  AI Agent
  * @note    X/Y/Z 轴输入距离(mm)，Yaw 轴输入角度(°)
  *          步距角 1.8°，32 细分，4mm 导程
  *          每圈脉冲数 = 360/1.8 × 32 = 6400
  *          X/Y/Z: pulse = mm × 6400/4 = mm × 1600
  *          Yaw:   pulse = ° × 6400/360
  */
#ifndef __MOTOR_H__
#define __MOTOR_H__

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ======================== 电机地址 ======================== */
#define MOTOR_ADDR_X    1U
#define MOTOR_ADDR_Y    2U
#define MOTOR_ADDR_Z    3U
#define MOTOR_ADDR_YAW  4U

/* ======================== 方向常量 ======================== */
#define MOTOR_DIR_CW    0x00U   /* 顺时针 */
#define MOTOR_DIR_CCW   0x01U   /* 逆时针 */

/* =========== 各轴正方向配置（待实测确定） =========== */
#define MOTOR_X_DIR_FORWARD     MOTOR_DIR_CCW
#define MOTOR_Y_DIR_FORWARD     MOTOR_DIR_CW
#define MOTOR_Z_DIR_FORWARD     MOTOR_DIR_CCW
#define MOTOR_YAW_DIR_FORWARD   MOTOR_DIR_CCW

/* =========== 各轴转速 RPM（可独立调节） =========== */
#define MOTOR_X_RPM     1500U
#define MOTOR_Y_RPM     1500U
#define MOTOR_Z_RPM     1500U
#define MOTOR_YAW_RPM   200U

/* =========== 上位机原点 → 电机零点 偏移量（待实测） =========== */
#define HOME_OFFSET_X_MM    -140.0f   /* 上位机原点在电机坐标系下的 X 坐标 */
#define HOME_OFFSET_Y_MM    -299.0f   /* 上位机原点在电机坐标系下的 Y 坐标 */

/* =========== Z 轴升降位置 =========== */
#define Z_HEIGHT_RAISE_MM   0.0f  /* 抬起高度（距离零点） */
#define Z_HEIGHT_LOWER_MM   13.0f   /* 放下高度（距离零点） */

/* ================== 运动参数 ================== */
#define MOTOR_ACCEL         0x96U       /* 加速度系数（默认 150） */
#define MOTOR_FRAME_INTERVAL_MS  10U    /* 帧间间隔 ≥10ms */
#define MOTOR_TIMEOUT_MS    20000U      /* 单次等待超时 20s */
#define MOTOR_RETRY_MAX     1U          /* 超时后重发次数 */

/* ================== 单位枚举 ================== */
typedef enum {
    UNIT_MM  = 0,   /**< 毫米 — X/Y/Z 轴 */
    UNIT_DEG = 1    /**< 角度 — Yaw 轴 */
} MotorUnit;

/* ================== 公共 API ================== */

/**
  * @brief  初始化全部电机（依次使能 → 清零四轴）
  * @note   上电后调用一次即可
  */
void Motor_Init(void);

/**
  * @brief  使能指定电机
  * @param  addr  电机地址
  * @retval 0=成功, -1=失败
  */
int Motor_Enable(uint8_t addr);

/**
  * @brief  清零指定电机（当前位置设为绝对零点）
  * @param  addr  电机地址
  * @retval 0=成功, -1=失败
  */
int Motor_Zero(uint8_t addr);

/**
  * @brief  发送绝对定位指令（非阻塞）
  * @param  addr  电机地址
  * @param  value 目标值：X/Y/Z 传 mm，Yaw 传 °
  * @param  unit  单位（UNIT_MM / UNIT_DEG）
  * @retval 0=发送成功（需调用 Motor_WaitMoveDone）, 1=已在目标位无需移动, -1=发送失败
  * @note   只发送不等待，适合多轴同时启动
  *         内部已包含 ≥10ms 帧间间隔，连续调用时无需额外延时
  *         返回 1 时表示该轴已在目标位置，上层应跳过 Motor_WaitMoveDone
  */
int Motor_SendMoveTo(uint8_t addr, float value, MotorUnit unit);

/**
  * @brief  等待指定电机到位（阻塞）
  * @param  addr  电机地址
  * @retval 0=到位成功, -1=超时
  * @note   等待电机回传 {addr} FD 9F 6B，超时 20s
  */
int Motor_WaitMoveDone(uint8_t addr);

/**
  * @brief  并行等待多个电机到位（阻塞）
  * @param  addrs      电机地址数组
  * @param  count      电机数量
  * @param  timeout_ms 总超时（从调用开始计时）
  * @retval 0=全部到位, -1=超时
  * @note   应答可乱序到达，按地址匹配
  *         适合共享总线场景下多轴并行运动后的等待
  */
int Motor_WaitAllDone(const uint8_t *addrs, uint8_t count, uint32_t timeout_ms);

/**
  * @brief  急停指定电机
  * @param  addr  电机地址
  * @retval 0=成功, -1=失败
  */
int Motor_Stop(uint8_t addr);

#ifdef __cplusplus
}
#endif

#endif /* __MOTOR_H__ */
