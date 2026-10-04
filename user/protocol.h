/**
  * @file    protocol.h
  * @brief   UART1 上位机通信协议解析模块
  * @author  AI Agent
  * @note    帧格式：AA + CMD + payload + BB
  *          坐标均为 int16 大端，值 = 原始值 / 100.0f
  */
#ifndef __PROTOCOL_H__
#define __PROTOCOL_H__

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ======================== 帧边界 ======================== */
#define FRAME_HEAD  0xAAU
#define FRAME_TAIL  0xBBU

/* ======================== 命令码 ======================== */
#define CMD_READY   0x00U   /* 上电就绪（MCU → 上位机） */
#define CMD_DATA    0x01U   /* 坐标指令（上位机 → MCU） */
#define CMD_DONE    0x02U   /* 完成确认（MCU → 上位机） */

/* ======================== 帧长度 ======================== */
#define FRAME_LEN_READY  4U    /* AA 00 mode BB */
#define FRAME_LEN_DATA   17U   /* AA 01 X val(2) Y val(2) X val(2) Y val(2) val(2) BB */
#define FRAME_LEN_DONE   3U    /* AA 02 BB */

/* 接收缓冲区最大长度 */
#define FRAME_BUF_MAX    17U

/* ======================== 坐标解析结构体 ======================== */

/**
  * @brief  上位机发来的运动指令（已解析为浮点数）
  */
typedef struct {
    float x_grab;    /**< 抓取点 X (mm) */
    float y_grab;    /**< 抓取点 Y (mm) */
    float x_place;   /**< 放置点 X (mm) */
    float y_place;   /**< 放置点 Y (mm) */
    float yaw;       /**< 旋转角度 (°) */
    uint8_t valid;   /**< 帧解析是否成功 */
} MoveCommand;

/* ======================== 公共 API ======================== */

/**
  * @brief  初始化协议模块并发送 READY 帧
  * @param  mode  工作模式（1/2/3，来自 PB4/PB5/PB6 按键）
  */
void Protocol_Init(uint8_t mode);

/**
  * @brief  向状态机喂入一个字节（由 UART1 中断回调调用）
  * @param  byte  接收到的字节
  */
void Protocol_FeedByte(uint8_t byte);

/**
  * @brief  查询是否收到完整帧
  * @retval 0=没有, 1=有待处理帧
  * @note   调用 Protocol_GetCommand() 后自动清除标志
  */
int Protocol_IsFrameReady(void);

/**
  * @brief  解析当前帧为 MoveCommand
  * @param  cmd  输出参数，填充解析结果
  * @retval 0=成功, -1=无效帧
  * @note   调用后自动清除帧就绪标志
  */
int Protocol_GetCommand(MoveCommand *cmd);

/**
  * @brief  发送完成确认帧 AA 02 BB
  */
void Protocol_SendDone(void);

#ifdef __cplusplus
}
#endif

#endif /* __PROTOCOL_H__ */
