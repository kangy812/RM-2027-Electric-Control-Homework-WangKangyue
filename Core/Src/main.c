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

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "Servo.h"
#include "OLED.h"
#include <stdio.h>

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* ---- 按键定义 ----
 * 两个按键都接成"一端接引脚、一端接 GND"，引脚用内部上拉：
 *   松开 = 高电平(1)，按下 = 低电平(0)
 *
 *   PA1 -> 角度 +15°
 *   PA2 -> 角度 -15°
 */
#define KEY_PORT          GPIOA
#define KEY_PIN_UP        GPIO_PIN_1     /* PA1：+15° */
#define KEY_PIN_DOWN      GPIO_PIN_2     /* PA2：-15° */

#define KEY_DEBOUNCE_MS   20             /* 消抖时间：电平要稳定 20ms 才算数 */
#define ANGLE_STEP        15.0f          /* 每按一次改变的角度 */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim3;

UART_HandleTypeDef huart1;

/* USER CODE BEGIN PV */
uint32_t tick = 0;

/* 自由累加的毫秒计数：在 TIM3 的 1ms 中断里 ++，不会归零。
 * 用来做按键消抖和序列节拍，比 tick（0~499 循环）好用。 */
static volatile uint32_t sysTickMs = 0;

/* 一个按键的运行状态 */
typedef struct
{
  uint16_t pin;      /* 引脚号 */
  uint8_t  raw;      /* 上次采样到的原始电平（1=松开 0=按下） */
  uint8_t  stable;   /* 消抖后确认的稳定电平 */
  uint32_t rawMs;    /* 原始电平上一次变化的时刻，单位 ms */
} Key_t;

static Key_t keyUp   = { KEY_PIN_UP,   1, 1, 0 };
static Key_t keyDown = { KEY_PIN_DOWN, 1, 1, 0 };

static float   curAngle   = 90.0f;  /* 当前目标角度 */
static uint8_t manualMode = 0;      /* 0 = 自动跑五角度序列，1 = 按键控制 */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_TIM2_Init(void);
static void MX_TIM3_Init(void);
static void MX_USART1_UART_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/**
 * @brief  printf 重定向到 USART1（GCC 用 _write，不用 fputc）
 * @note  PA9 = USART1_TX -> CH340 的 RXD
 *        PA10 = USART1_RX <- CH340 的 TXD
 *        串口助手设 115200 / 8 数据位 / 无校验 / 1 停止位
 */
int _write(int file, char *ptr, int len)
{
  (void)file;
  HAL_UART_Transmit(&huart1, (uint8_t *)ptr, (uint16_t)len, HAL_MAX_DELAY);
  return len;
}

/**
 * @brief  按键扫描——"计时消抖"，不是"延时消抖"
 * @param  k 按键状态结构体
 * @retval 1 = 本次检测到一次"按下"（下降沿），0 = 无事件
 *
 * 为什么不用 HAL_Delay(20) 消抖：
 *   延时消抖会把主循环卡住 20ms，舵机 PWM 不受影响（那是硬件输出），
 *   但按键连续按、OLED 刷屏时会互相拖。这里改成看"电平稳定了多久"，
 *   完全不阻塞，主循环该干嘛干嘛。
 *
 * 逻辑很朴素，三句话：
 *   1. 电平一变，就记下"什么时刻变的"并重新开始计时；
 *   2. 电平连续 20ms 没再变，才承认它是真的；
 *   3. 承认下来的值是低电平，说明刚被按下 -> 返回 1。
 */
static uint8_t Key_Update(Key_t *k)
{
  uint8_t v = (HAL_GPIO_ReadPin(KEY_PORT, k->pin) == GPIO_PIN_RESET) ? 0 : 1;

  if (v != k->raw)                       /* 还在抖：重新计时，先不认 */
  {
    k->raw   = v;
    k->rawMs = sysTickMs;
  }
  else if ((sysTickMs - k->rawMs) >= KEY_DEBOUNCE_MS && v != k->stable)
  {
    k->stable = v;                       /* 稳定够久了，认下来 */
    if (v == 0) return 1;                /* 是按下（下降沿）才算一次事件 */
  }
  return 0;
}

/**
 * @brief  统一切角度：限幅 -> 驱动舵机 -> 刷新 OLED -> 打印串口
 * @note   自动序列和手动按键都走这一个出口，省得两处代码各写一遍、后面改漏一头。
 */
static void ApplyAngle(float a)
{
  if (a < 0.0f)   a = 0.0f;              /* 限幅：不超出 0~180，否则顶机械限位 */
  if (a > 180.0f) a = 180.0f;
  curAngle = a;

  Servo_SetAngle(a);

  uint16_t ccr = Servo_AngleToCCR(a);
  Servo_ShowOnOLED(a, ccr);
  printf("Target:%3d deg  CCR=%4d  Pulse=%d.%03d ms\r\n",
         (int)a, (int)ccr, (int)(ccr / 1000), (int)(ccr % 1000));
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
  MX_TIM2_Init();
  MX_TIM3_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
  OLED_Init();

  /* 把自检的详细结果打到串口，比数灯可靠 */
  printf("\r\n=== servo_lab boot ===\r\n");
  printf("OLED probe: c1=%d c2=%d c3=%d c4=%d | result=%d addr=0x%02X\r\n",
         OLED_ProbeLog[0], OLED_ProbeLog[1], OLED_ProbeLog[2], OLED_ProbeLog[3],
         OLED_DetectResult, OLED_AckAddr);

  /* ---- OLED 自检结果，用板载灯闪的次数报告 ----
   *   闪 1 次 = 正常接法 + 地址 0x78（应该能正常显示）
   *   闪 2 次 = 正常接法 + 地址 0x7A（已自动切换，能显示）
   *   闪 3 次 = SCL/SDA 接反了，但驱动已自动纠正，能显示
   *   闪 4 次 = 接反 + 地址 0x7A，能显示
   *   闪 5 次 = 4 种组合全无应答（供电 / 断线 / 模块坏）
   */
  {
    uint8_t n = (OLED_DetectResult != 0) ? OLED_DetectResult : 5;
    for (uint8_t k = 0; k < n; k++)
    {
      HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_RESET);  /* 亮 */
      HAL_Delay(150);
      HAL_GPIO_WritePin(GPIOC, GPIO_PIN_13, GPIO_PIN_SET);    /* 灭 */
      HAL_Delay(400);
    }
  }

  HAL_TIM_Base_Start_IT(&htim3);
  HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);

  /* ---- 按键 PA1 / PA2 初始化：输入 + 内部上拉 ----
   * CubeMX 里也能配（GPIO_Input + Pull-up），这里手写有两个好处：
   *   1. 不用重新 GENERATE CODE，也就不会把 .ld 里 5 处 READONLY 又改回来；
   *   2. 引脚定义和代码放在一起，看代码就知道接哪。
   * 上拉是"内部"的，所以按键另一端直接接 GND 就行，不用外接电阻。
   */
  {
    GPIO_InitTypeDef g = {0};
    __HAL_RCC_GPIOA_CLK_ENABLE();
    g.Pin  = KEY_PIN_UP | KEY_PIN_DOWN;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(KEY_PORT, &g);
  }

  OLED_Clear();
  ApplyAngle(90.0f);          /* 开机先摆到中位，屏幕立刻有内容可看 */
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    static const float seq[5] = {0.0f, 45.0f, 90.0f, 135.0f, 180.0f};
    static uint8_t  seqStep = 0;
    static uint32_t seqMs   = 0;

    /* ---- 第一段：自动演示 ----
     * 每 1000ms 走一步五角度序列，这是作业要求的 5 个角度。
     * 注意这里用 sysTickMs 比较，而不是 HAL_Delay(1000)：
     * 延时会把主循环卡死，按键就按不灵了；用计时比较，1 秒内随时能响应按键。
     */
    if (!manualMode && (sysTickMs - seqMs) >= 1000)
    {
      seqMs = sysTickMs;
      ApplyAngle(seq[seqStep]);
      seqStep = (uint8_t)((seqStep + 1) % 5);
    }

    /* ---- 第二段：按键 ----
     * 只要按了任意一个键，就永久切到手动模式，之后不再自己乱动。
     * 想回到自动演示，按一下板子的 RESET 即可。
     */
    if (Key_Update(&keyUp))               /* PA1：+15° */
    {
      manualMode = 1;
      ApplyAngle(curAngle + ANGLE_STEP);
    }

    if (Key_Update(&keyDown))             /* PA2：-15° */
    {
      manualMode = 1;
      ApplyAngle(curAngle - ANGLE_STEP);
    }
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

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 71;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 19999;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_PWM_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */
  HAL_TIM_MspPostInit(&htim2);

}

/**
  * @brief TIM3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM3_Init(void)
{

  /* USER CODE BEGIN TIM3_Init 0 */

  /* USER CODE END TIM3_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM3_Init 1 */

  /* USER CODE END TIM3_Init 1 */
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 71;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 999;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim3, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM3_Init 2 */

  /* USER CODE END TIM3_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : LED_Pin */
  GPIO_InitStruct.Pin = LED_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM3) {
    sysTickMs++;                          /* 自由累加的毫秒计数，给按键消抖和序列节拍用 */
    if (++tick >= 500) {
      tick = 0;
      HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
    }
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
