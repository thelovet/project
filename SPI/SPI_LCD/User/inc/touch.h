#ifndef __TOUCH_H__
#define __TOUCH_H__

#include "ch32v20x.h"

#define CST816S_REG_LP_SCAN_TH       0xF5
#define CST816S_LP_SCAN_THRESHOLD     32U

/**
 * @brief CST816S 触摸引脚定义
 */
#define TOUCH_RST_RCC     RCC_APB2Periph_GPIOB
#define TOUCH_RST_PORT    GPIOB
#define TOUCH_RST_PIN     GPIO_Pin_8          // RST1: PB8

#define TOUCH_INT_RCC     RCC_APB2Periph_GPIOA
#define TOUCH_INT_PORT    GPIOA
#define TOUCH_INT_PIN     GPIO_Pin_8          // INT: PA8 (EXTI8)

/**
 * @brief 软件I2C引脚 (SCL:PB10, SDA:PB11)
 */
#define TOUCH_SCL_PORT    GPIOB
#define TOUCH_SCL_PIN     GPIO_Pin_10
#define TOUCH_SDA_PORT    GPIOB
#define TOUCH_SDA_PIN     GPIO_Pin_11

/**
 * @brief CST816S I2C地址
 */
#define CST816S_ADDR      0x15                // 7位地址

/**
 * @brief CST816S 内部寄存器地址
 */
#define CST816S_REG_GESTURE    0x01           // 手势ID
#define CST816S_REG_FINGERNUM  0x02           // 手指数量
#define CST816S_REG_XH         0x03           // X坐标高4位 [11:8]
#define CST816S_REG_XL         0x04           // X坐标低8位 [7:0]
#define CST816S_REG_YH         0x05           // Y坐标高4位 [11:8]
#define CST816S_REG_YL         0x06           // Y坐标低8位 [7:0]
#define CST816S_REG_CHIPID     0xA7           // 芯片ID (预期0xB4或0xB5)

/**
 * @brief CST816S 控制寄存器
 */
#define CST816S_REG_IRQCTL      0xFA           // 中断控制 (必须配置!)
//   0xFA: [7]=使能 [6]=触摸产生中断方式 [5:0]=保留
#define CST816S_IRQ_ENABLE      0x40           // 使能并周期产生中断

#define CST816S_REG_AUTOSLEEP   0xFE           // 自动休眠控制
#define CST816S_SLEEP_ENABLE    0x00           // 启用自动休眠(无触摸约2秒后休眠)
#define CST816S_SLEEP_DISABLE   0x01           // 禁用自动休眠(始终保持活跃)

/**
 * @brief 触摸状态结构体
 */
typedef struct {
    uint8_t  pressed;       // 1=有触摸, 0=无触摸
    uint16_t x;             // X坐标
    uint16_t y;             // Y坐标
} TouchState;

/**
 * @brief INT标志位 (ISR置位)
 */
extern volatile uint8_t g_touch_int_flag;
extern uint8_t g_touch_pressed;

/**
 * @brief 函数声明
 */
void    Touch_Init(void);
void    Touch_WakeResumeInit(void);              // 唤醒确认前恢复I2C，不复位CST816S
uint8_t Touch_IsPressed(void);                    // INT trigger: press->1, release->0, cache coords
void    Touch_Scan(TouchState *state);            // read cached coords (no I2C)
void    Touch_UpdateCoords(void);                 // poll I2C for real-time coords (during hold)
void    CST816S_SleepEnable(void);                // 启用自动休眠 (待机前调用)
void    CST816S_SleepDisable(void);               // 禁用自动休眠 (唤醒后调用)

/**
 * @brief Standby唤醒配置
 */
void    Touch_StandbyWakeupConfig(void);          // 配置PA8 EXTI事件唤醒
void    Touch_WaitWakeInputsReleased(void);        // 等待触摸完全释放

#endif
