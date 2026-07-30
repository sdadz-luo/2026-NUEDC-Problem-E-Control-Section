/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usart.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "motor.h"
#include "protocol.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define GAME_TIMEOUT_MS      5000U   /* 游戏完成超时 */
#define COMPLETE_DURATION_MS 5000U   /* 完成模式持续时间 */
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
uint8_t g_mode;          /* 工作模式：1=模式1, 2=模式2 */
uint8_t uart1_rx_byte;   /* UART1 单字节中断接收缓冲 */

typedef enum {
    SYS_IDLE,       /* 等待第一个坐标帧 */
    SYS_WAIT_NEXT,  /* 已发 DONE，等待下一条指令或超时 */
    SYS_COMPLETE    /* 游戏完成，闪烁 5s */
} SysState;
SysState sys_state;
uint32_t tick_done;      /* DONE 帧发送时刻 */
uint32_t tick_complete;  /* 进入完成模式时刻 */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */
static void pick_and_place(MoveCommand *cmd);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/**
  * @brief  执行一次 pick-and-place 动作序列
  * @param  cmd  已解析的坐标指令
  * @note   阻塞执行，所有步骤串行等待到位
  */
static void pick_and_place(MoveCommand *cmd)
{
    /* 1. 移动到抓取点（先发后等，确保双轴同时运动） */
    {
        int x_ret = Motor_SendMoveTo(MOTOR_ADDR_X, cmd->x_grab + HOME_OFFSET_X_MM, UNIT_MM);
        int y_ret = Motor_SendMoveTo(MOTOR_ADDR_Y, cmd->y_grab + HOME_OFFSET_Y_MM, UNIT_MM);
        if (x_ret == 0) Motor_WaitMoveDone(MOTOR_ADDR_X);
        if (y_ret == 0) Motor_WaitMoveDone(MOTOR_ADDR_Y);
    }

    /* 2. Z 轴落下 */
    if (Motor_SendMoveTo(MOTOR_ADDR_Z, Z_HEIGHT_LOWER_MM, UNIT_MM) == 0)
        Motor_WaitMoveDone(MOTOR_ADDR_Z);

    /* 3. 电磁铁吸合 */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, GPIO_PIN_RESET);
    HAL_Delay(500);

    /* 4. Z 轴抬起 */
    if (Motor_SendMoveTo(MOTOR_ADDR_Z, Z_HEIGHT_RAISE_MM, UNIT_MM) == 0)
        Motor_WaitMoveDone(MOTOR_ADDR_Z);

    /* 5. 移动到放置点 */
    {
        int x_ret  = Motor_SendMoveTo(MOTOR_ADDR_X,   cmd->x_place + HOME_OFFSET_X_MM, UNIT_MM);
        int y_ret  = Motor_SendMoveTo(MOTOR_ADDR_Y,   cmd->y_place + HOME_OFFSET_Y_MM, UNIT_MM);
        int yaw_ret = Motor_SendMoveTo(MOTOR_ADDR_YAW, cmd->yaw,                      UNIT_DEG);
        if (x_ret  == 0) Motor_WaitMoveDone(MOTOR_ADDR_X);
        if (y_ret  == 0) Motor_WaitMoveDone(MOTOR_ADDR_Y);
        if (yaw_ret == 0) Motor_WaitMoveDone(MOTOR_ADDR_YAW);
    }

    /* 6. Z 轴落下 */
    if (Motor_SendMoveTo(MOTOR_ADDR_Z, Z_HEIGHT_LOWER_MM, UNIT_MM) == 0)
        Motor_WaitMoveDone(MOTOR_ADDR_Z);

    /* 7. 电磁铁释放 */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, GPIO_PIN_SET);

    /* 8. Z 轴抬起 */
    if (Motor_SendMoveTo(MOTOR_ADDR_Z, Z_HEIGHT_RAISE_MM, UNIT_MM) == 0)
        Motor_WaitMoveDone(MOTOR_ADDR_Z);

    /* 9. Yaw 归零 */
    if (Motor_SendMoveTo(MOTOR_ADDR_YAW, 0.0f, UNIT_DEG) == 0)
        Motor_WaitMoveDone(MOTOR_ADDR_YAW);
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART1_UART_Init();
  MX_USART3_UART_Init();
  /* USER CODE BEGIN 2 */

	/* 上电检测 PB5 电平决定工作模式 */
	g_mode = (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_5) == GPIO_PIN_SET) ? 1 : 2;

	Motor_Init();                                // 使能+清零四轴

	Protocol_Init(g_mode);                       // 发送 READY 帧
	HAL_UART_Receive_IT(&huart1, &uart1_rx_byte, 1);  // 启动单字节接收

	sys_state = SYS_IDLE;                        // 初始状态

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    switch (sys_state)
    {
    case SYS_IDLE:
    case SYS_WAIT_NEXT:
        /* 收到坐标帧 → 执行动作 → 发 DONE → 看门狗计时 */
        if (Protocol_IsFrameReady())
        {
            MoveCommand cmd;
            if (Protocol_GetCommand(&cmd) == 0 && cmd.valid)
            {
                pick_and_place(&cmd);
                Protocol_SendDone();
                tick_done  = HAL_GetTick();
                sys_state  = SYS_WAIT_NEXT;
            }
        }

        /* 5s 内未收到下一条指令 → 游戏完成 */
        if (sys_state == SYS_WAIT_NEXT
            && HAL_GetTick() - tick_done > GAME_TIMEOUT_MS)
        {
            /* 四轴归零（仅发送，不等待到位） */
            Motor_SendMoveTo(MOTOR_ADDR_X,   0.0f, UNIT_MM);
            Motor_SendMoveTo(MOTOR_ADDR_Y,   0.0f, UNIT_MM);
            Motor_SendMoveTo(MOTOR_ADDR_Z,   0.0f, UNIT_MM);
            Motor_SendMoveTo(MOTOR_ADDR_YAW, 0.0f, UNIT_DEG);

            tick_complete = HAL_GetTick();
            sys_state     = SYS_COMPLETE;
        }
        break;

    case SYS_COMPLETE:
        {
            uint32_t elapsed = HAL_GetTick() - tick_complete;
            if (elapsed < COMPLETE_DURATION_MS)
            {
                /* 0.5Hz 闪烁：亮 0.5s / 灭 0.5s */
                uint8_t on = ((elapsed % 1000) < 500);
                HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
                HAL_GPIO_WritePin(GPIOA, GPIO_PIN_3,  on ? GPIO_PIN_SET : GPIO_PIN_RESET);
            }
            else
            {
                /* 5s 后关闭，保持静止 */
                HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);
                HAL_GPIO_WritePin(GPIOA,  GPIO_PIN_3,  GPIO_PIN_RESET);
            }
        }
        break;
    }

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* UART1 接收回调：逐字节喂入协议状态机 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1)
    {
        Protocol_FeedByte(uart1_rx_byte);
        HAL_UART_Receive_IT(&huart1, &uart1_rx_byte, 1);  /* 重新启动接收 */
    }
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
