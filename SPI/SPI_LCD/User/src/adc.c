#include "adc.h"
#include "ch32v20x_adc.h"
#include "debug.h"

/**
 * @brief  打开电压测量开关（PB3置低）
 */
static void VOLT_SW_Open(void)
{
    GPIO_ResetBits(VOLT_SW_PORT, VOLT_SW_PIN);
}

/**
 * @brief  关闭电压测量开关（PB3置高）
 */
static void VOLT_SW_Close(void)
{
    GPIO_SetBits(VOLT_SW_PORT, VOLT_SW_PIN);
}

/**
 * @brief  初始化电池电压检测ADC和PA15测量开关
 *         PA1(ADC1_Channel_1) 模拟输入
 *         PA15(VOLT_SW) 推挽输出(低电平=打开测量, 高电平=断开测量)
 */
void BAT_ADC_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    ADC_InitTypeDef ADC_InitStructure = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_ADC1, ENABLE);

    // PA15: 电压测量开关(推挽输出, 默认断开)
    GPIO_InitStructure.GPIO_Pin = VOLT_SW_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(VOLT_SW_PORT, &GPIO_InitStructure);
    VOLT_SW_Close();  // 默认关闭

    // PA1: ADC模拟输入
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_1;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AIN;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    ADC_DeInit(ADC1);
    ADC_InitStructure.ADC_Mode = ADC_Mode_Independent;
    ADC_InitStructure.ADC_ScanConvMode = DISABLE;
    ADC_InitStructure.ADC_ContinuousConvMode = DISABLE;
    ADC_InitStructure.ADC_ExternalTrigConv = ADC_ExternalTrigConv_None;
    ADC_InitStructure.ADC_DataAlign = ADC_DataAlign_Right;
    ADC_InitStructure.ADC_NbrOfChannel = 1;
    ADC_InitStructure.ADC_OutputBuffer = ADC_OutputBuffer_Enable;
    ADC_InitStructure.ADC_Pga = ADC_Pga_1;
    ADC_Init(ADC1, &ADC_InitStructure);

    // 使能内部温度传感器和VREFINT
    ADC_TempSensorVrefintCmd(ENABLE);

    ADC_Cmd(ADC1, ENABLE);

    ADC_ResetCalibration(ADC1);
    while(ADC_GetResetCalibrationStatus(ADC1));
    ADC_StartCalibration(ADC1);
    while(ADC_GetCalibrationStatus(ADC1));
    
    // 稳定ADC
    for(uint8_t i = 0; i < 3; i++) {
        BAT_ADC_ReadRaw();
    }
}

/**
 * @brief  切换ADC通道并丢弃前几次采样
 * @param channel ADC通道
 */
static void ADC_SwitchChannel(uint8_t channel)
{
    ADC_RegularChannelConfig(ADC1, channel, 1, ADC_SampleTime_239Cycles5);
    // 丢弃前几次采样，让采样电容充放电稳定
    for (uint8_t i = 0; i < 4; i++) {
        BAT_ADC_ReadRaw();
    }
}

/**
 * @brief  读取ADC原始值
 * @return 12位ADC采样值 (0-4095)
 */
uint16_t BAT_ADC_ReadRaw(void)
{
    uint32_t timeout = 100000;  /* 仿真时ADC不工作，超时退出避免死循环 */
    ADC_SoftwareStartConvCmd(ADC1, ENABLE);
    while(!ADC_GetFlagStatus(ADC1, ADC_FLAG_EOC)) {
        if (--timeout == 0) return 0;
    }
    return ADC_GetConversionValue(ADC1);
}

/**
 * @brief  读取VREFINT，计算实际VDDA
 * @return 实际VDDA电压值 (单位: mV)
 *         原理: VDDA = VREFINT_NOMINAL × 4096 / VREFINT_RAW
 */
static uint16_t ADC_GetVDDA(void)
{
    ADC_SwitchChannel(VREFINT_CHANNEL);
    
    uint32_t sum = 0;
    for (uint8_t i = 0; i < 8; i++) {
        sum += BAT_ADC_ReadRaw();
    }
    uint16_t vrefint_raw = (uint16_t)(sum / 8);
    
    // VDDA = 1200mV × 4096 / vrefint_raw
    uint32_t vdda = ((uint32_t)VREFINT_NOMINAL * BAT_ADC_RESOLUTION) / vrefint_raw;
    return (uint16_t)vdda;
}

/**
 * @brief  读取电池电压（考虑分压电路）
 * @return 电池电压值 (单位: mV)
 *         分压电路: 电池电压 = ADC输入电压 × 2
 *         满电4.2V → ADC输入2.1V
 *         测量流程: 先读VREFINT校准VDDA → 切换通道 → 采样
 */
uint16_t BAT_ADC_ReadVoltage(void)
{
    VOLT_SW_Open();
    Delay_Ms(50);
    
    // 读取VREFINT获取实际VDDA（电池供电时VDDA会变化）
    uint16_t vdda = ADC_GetVDDA();
    
    // 切换到电池ADC通道
    ADC_SwitchChannel(BAT_ADC_CHANNEL);
    
    uint32_t adc_sum = 0;
    for (uint8_t i = 0; i < BAT_ADC_SAMPLE_CNT; i++) {
        adc_sum += BAT_ADC_ReadRaw();
        Delay_Us(100);
    }
    uint32_t adc_avg = adc_sum / BAT_ADC_SAMPLE_CNT;
    
    VOLT_SW_Close();
    
    // 使用实际VDDA计算，而非固定参考电压
    uint32_t voltage = (adc_avg * vdda * BAT_DIVIDER_RATIO * BAT_ADC_CAL_MUL) 
                       / (BAT_ADC_RESOLUTION * BAT_ADC_CAL_DIV);
    return (uint16_t)voltage;
}