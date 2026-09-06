#include "key.h"
#include "timer.h"

volatile uint8_t key_status = 0;        // 按键状态标志
volatile uint32_t key_press_start = 0;   // 按下开始时间
volatile uint32_t key_cooldown = 0;      // 上次触发时间（冷却期起点）
volatile uint8_t key_holding = 0;        // 按键正在按住
volatile uint8_t long_press_triggered = 0; // 长按已触发（防止重复）


#define KEY_COOLDOWN_MS      50            // 冷却时间（ms），改这一个值即可
#define KEY_MIN_PRESS_MS     5              // 最短按下时间（0.1ms），低于此视为抖动
#define KEY_LONG_PRESS_MS    500            // 长按时间阈值（ms）
#define KEY_COOLDOWN_TIME   (KEY_COOLDOWN_MS * 1000)  // 自动计算（μs）
#define KEY_MIN_PRESS_TIME   (KEY_MIN_PRESS_MS * 100)  // 自动计算（μs）
#define KEY_LONG_PRESS_TIME  (KEY_LONG_PRESS_MS * 1000) // 自动计算（μs）
/***********************************************************************
 * @brief  PA0外部中断初始化，用于按键检测（支持短按和长按）
 **********************************************************************/
void KEY_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    EXTI_InitTypeDef EXTI_InitStructure;
    NVIC_InitTypeDef NVIC_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_AFIO, ENABLE);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPD;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    GPIO_EXTILineConfig(GPIO_PortSourceGPIOA, GPIO_PinSource0);

    EXTI_InitStructure.EXTI_Line = EXTI_Line0;
    EXTI_InitStructure.EXTI_Mode = EXTI_Mode_Interrupt;
    EXTI_InitStructure.EXTI_Trigger = EXTI_Trigger_Rising_Falling;
    EXTI_InitStructure.EXTI_LineCmd = ENABLE;
    EXTI_Init(&EXTI_InitStructure);

    NVIC_InitStructure.NVIC_IRQChannel = EXTI0_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 2;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 2;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
}
/**
 * @brief  获取按键状态。
 * @return 按键状态
 */

uint8_t Get_Key_Status(void)//获取按键状态
{
    return key_status;
}
/**
 * @brief  清除按键状态。
 */

void Clear_Key_Status(void)//清除按键状态
{
    key_status = 0;
}
/**
 * @brief  重置按键状态(重新开始/模式切换时调用)。
 */

void KEY_Reset(void)//重置按键状态
{
    key_status = 0;
    key_press_start = 0;
    key_cooldown = 0;
    key_holding = 0;
    long_press_triggered = 0;
}

/*********************************************************************
 * @fn      EXTI0_IRQHandler
 *
 * @brief   按下记录时间 → 主循环轮询长按 → 释放时短按判定
 *          长按：按住超时即触发，不等待释放
 *          短按：释放时判定（且未曾触发长按）
 **********************************************************************/
void EXTI0_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
/**
 * @brief  按键外部中断(EXTI0)处理。
 * @note   标准 ISR, 记录按键触发并清除中断标志。
 */
void EXTI0_IRQHandler(void)
{
    if(EXTI_GetITStatus(EXTI_Line0) != RESET)
    {
        uint32_t now = GetTimerCount();
        uint8_t pin = GPIO_ReadInputDataBit(GPIOA, GPIO_Pin_0);

        // 冷却检查
        if((now - key_cooldown) < KEY_COOLDOWN_TIME)
        {
            EXTI_ClearITPendingBit(EXTI_Line0);
            return;
        }

        if(pin == Bit_SET)
        {
            // 按下：记录时间，开始计时
            key_press_start = now;
            key_holding = 1;
            long_press_triggered = 0;
        }
        else
        {
            // 释放：若长按未触发，判定短按
            if(key_holding && !long_press_triggered)
            {
                uint32_t press_duration = now - key_press_start;
                if(press_duration >= KEY_MIN_PRESS_TIME)
                {
                    key_status = KEY_SHORT_PRESS;
                    key_cooldown = now;
                }
            }
            key_holding = 0;
        }

        EXTI_ClearITPendingBit(EXTI_Line0);
    }
}

/*********************************************************************
 * @fn      KEY_Poll
 *
 * @brief   主循环轮询：按住超时触发长按
 **********************************************************************/
void KEY_Poll(void)
{
#if KEY_LONG_PRESS_ENABLE
    if(key_holding && !long_press_triggered)
    {
        uint32_t now = GetTimerCount();
        if((now - key_press_start) >= KEY_LONG_PRESS_TIME)
        {
            key_status = KEY_LONG_PRESS;
            long_press_triggered = 1;
            key_cooldown = now;
        }
    }
#endif
}
