/**
  * @file    protocol.c
  * @brief   UART1 上位机通信协议 — 状态机解析
  * @author  AI Agent
  * @note    依赖 huart1（已由 CubeMX 初始化）
  *          由 HAL_UART_RxCpltCallback 逐字节喂入
  */
#include "protocol.h"
#include "usart.h"

/* ======================== 接收状态机 ======================== */
typedef enum {
    STATE_IDLE,       /* 等待帧头 0xAA */
    STATE_WAIT_CMD,   /* 已收到 AA，等待命令码 */
    STATE_WAIT_DATA   /* 已收到命令码，收剩余数据 */
} RxState;

static RxState rx_state = STATE_IDLE;
static uint8_t  rx_buf[FRAME_BUF_MAX];  /* 接收缓冲 */
static uint8_t  rx_idx;                 /* 当前写入位置 */
static uint8_t  rx_total_len;           /* 当前帧预期总长度 */
static volatile uint8_t rx_frame_ready; /* 完整帧就绪标志 */

/* ======================== 内部辅助 ======================== */

/**
  * @brief  将 2 字节大端 int16 转为有符号值，再 /100 得浮点
  * @param  p  指向 2 字节的指针
  * @retval float 值
  */
static float ParseCoord(const uint8_t *p)
{
    int16_t raw;

    raw = (int16_t)(((uint16_t)p[0] << 8) | p[1]);
    return (float)raw / 100.0f;
}

/* ======================== 公共接口 ======================== */

void Protocol_Init(uint8_t mode)
{
    uint8_t frame[4];

    /* 初始化状态机 */
    rx_state       = STATE_IDLE;
    rx_idx         = 0;
    rx_total_len   = 0;
    rx_frame_ready = 0;

    /* 组装并发送 READY 帧：AA 00 mode BB */
    frame[0] = FRAME_HEAD;
    frame[1] = CMD_READY;
    frame[2] = mode;
    frame[3] = FRAME_TAIL;

    HAL_UART_Transmit(&huart1, frame, sizeof(frame), 100);
}

void Protocol_FeedByte(uint8_t byte)
{
    switch (rx_state) {

    case STATE_IDLE:
        if (byte == FRAME_HEAD) {
            rx_buf[0] = byte;
            rx_idx    = 1;
            rx_state  = STATE_WAIT_CMD;
        }
        /* 非 AA 字节丢弃，保持 IDLE */
        break;

    case STATE_WAIT_CMD:
        rx_buf[1] = byte;
        rx_idx    = 2;

        /* 根据命令码确定帧总长 */
        switch (byte) {
        case CMD_READY:
            rx_total_len = FRAME_LEN_READY;
            rx_state     = STATE_WAIT_DATA;
            break;
        case CMD_DATA:
            rx_total_len = FRAME_LEN_DATA;
            rx_state     = STATE_WAIT_DATA;
            break;
        default:
            /* 非法命令码，复位 */
            rx_state = STATE_IDLE;
            break;
        }
        break;

    case STATE_WAIT_DATA:
        /* 防止溢出 */
        if (rx_idx >= FRAME_BUF_MAX) {
            rx_state = STATE_IDLE;
            break;
        }

        rx_buf[rx_idx++] = byte;

        /* 收满整帧 */
        if (rx_idx >= rx_total_len) {
            /* 校验帧尾 */
            if (rx_buf[rx_total_len - 1] == FRAME_TAIL) {
                rx_frame_ready = 1;
            }
            rx_state = STATE_IDLE;
        }
        break;
    }
}

int Protocol_IsFrameReady(void)
{
    return rx_frame_ready;
}

int Protocol_GetCommand(MoveCommand *cmd)
{
    if (cmd == NULL) {
        return -1;
    }

    if (!rx_frame_ready) {
        cmd->valid = 0;
        return -1;
    }

    /* 只处理 CMD_DATA 帧，其他命令码标记无效 */
    if (rx_buf[1] != CMD_DATA) {
        cmd->valid     = 0;
        rx_frame_ready = 0;
        return -1;
    }

    /*
     * CMD_DATA 帧结构 (17 字节)：
     * [0]AA [1]01 [2]X [3-4]Xg [5]Y [6-7]Yg [8]X [9-10]Xp [11]Y [12-13]Yp [14-15]Yaw [16]BB
     */
    cmd->x_grab  = ParseCoord(&rx_buf[3]);
    cmd->y_grab  = ParseCoord(&rx_buf[6]);
    cmd->x_place = ParseCoord(&rx_buf[9]);
    cmd->y_place = ParseCoord(&rx_buf[12]);
    cmd->yaw     = ParseCoord(&rx_buf[14]);
    cmd->valid   = 1;

    rx_frame_ready = 0;
    return 0;
}

void Protocol_SendDone(void)
{
    uint8_t frame[3];

    /* AA 02 BB */
    frame[0] = FRAME_HEAD;
    frame[1] = CMD_DONE;
    frame[2] = FRAME_TAIL;

    HAL_UART_Transmit(&huart1, frame, sizeof(frame), 100);
}
