#ifndef __ADC_H__
#define __ADC_H__

#include "ch32v20x.h"

// 电压测量开关GPIO
#define VOLT_SW_RCC         RCC_APB2Periph_GPIOA
#define VOLT_SW_PORT        GPIOA
#define VOLT_SW_PIN         GPIO_Pin_15

// 电池电压ADC通道
#define BAT_ADC_CHANNEL     ADC_Channel_1      // PA1对应ADC通道1
#define BAT_ADC_RESOLUTION  4096               // 12位ADC分辨率
#define BAT_DIVIDER_RATIO   2                  // 分压比: 电池电压到ADC输入为2:1
#define BAT_ADC_SAMPLE_CNT  16                 // 采样次数(多次采样取平均)

// 内部参考电压
#define VREFINT_CHANNEL     ADC_Channel_Vrefint  // 内部1.2V参考电压通道
#define VREFINT_NOMINAL     1200                 // 内部参考电压标称值(mV)

// 校准系数: 实际电压 = ADC读数 * CAL_MUL / CAL_DIV
#define BAT_ADC_CAL_MUL     100                // 校准分子
#define BAT_ADC_CAL_DIV     100                // 校准分母 (100=无校准)

/**
 * @brief  初始化电池电压检测ADC和PA15测量开关
 *         PA1(ADC1_Channel_1) 模拟输入
 *         PA15(VOLT_SW) 推挽输出(低电平=打开测量, 高电平=断开测量)
 */
void BAT_ADC_Init(void);

/**
 * @brief  读取ADC原始值
 * @return 12位ADC采样值 (0-4095)
 */
uint16_t BAT_ADC_ReadRaw(void);

/**
 * @brief  读取电池电压(考虑分压电路和校准)
 * @return 电池电压值 (单位: mV)
 *         分压电路: 电池电压 = ADC输入电压 × 2
 *         满电4.2V → ADC输入2.1V
 *         测量流程: 先读VREFINT校准VDDA → 切换通道 → 采样
 */
uint16_t BAT_ADC_ReadVoltage(void);

#endif
