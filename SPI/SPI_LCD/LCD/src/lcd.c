/*********************************************************************
 * File Name          : lcd.c
 * Author             : WCH
 * Version            : V1.0.0
 * Date               : 2023/8/15
 * Description        : file for lcd screen.
 *********************************************************************************
* Copyright (c) 2023 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/

#include "lcd.h"
#include "debug.h"
#include "spi.h"
#include "decompress.h"    // 解压缩算法头文件

#define LCD_SOFT_RESET          (0x01)
#define LCD_READ_ID             (0x04)
#define LCD_READ_DISPLAY_STATUS (0x09)
#define LCD_ON                  (0x28)
#define LCD_OFF                 (0x29)

#define LCD_SET_X               (0x2a)
#define LCD_SET_Y               (0x2B)
#define LCD_MEM_WRITE           (0x2C)
#define LCD_MEM_READ            (0x2e)

#define USE_HORIZONTAL 0  //设置横屏或者竖屏显示 0或1为竖屏 2或3为横屏
u8 shared_buffer[FILL_BUFFER_SIZE] = { 0 };


void FastFill_16bit_Repeat(uint8_t *buf, uint32_t len, uint8_t color_l, uint8_t color_h);//高效填充重复的16位颜色到缓冲区（自动适应长度）
void LCD_drawImageWithSize_DMA(uint16_t count,uint8_t pre_count);//从flash读取数据发送到LCD屏幕
void LCD_DMA_Write_Buffer(uint16_t color, uint32_t total_size);//将缓冲区数据发送到LCD屏幕


/*********************************************************************
 * @fn      LCD_WR_REG
 *
 * @brief   Write an 8-bit command to the LCD screen  
 * @brief   向液晶显示屏写入 8 位命令字
 * 
 * @param   data - Command value to be written
 *
 * @return  none
 */
void LCD_WR_REG(uint8_t data)
{
    //LCD_CS_CLR;
    LCD_DC_CLR;
    SPI2_Write(data);
    //LCD_CS_SET;
}

/*********************************************************************
 * @fn      LCD_WR_DATA
 *
 * @brief   Write an 8-bit data to the LCD screen  
 * @brief   向液晶显示屏写入 8 位数据
 * 
 * @param   data - the data to write
 * 
 */
void LCD_WR_DATA(uint8_t data)
{
    //LCD_CS_CLR;
    LCD_DC_SET;
    SPI2_Write(data);
    //LCD_CS_SET;
}


/******************************************************************************
      函数说明：LCD写入数据
      入口数据：dat 写入的数据
      返回值：  无
******************************************************************************/
void LCD_WR_DATA_16Bit(uint16_t dat)
{
	LCD_DC_SET;
    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_TXE) == RESET);//等待发送缓冲区为空
    SPI_I2S_SendData(SPI2, dat >> 8);      // 发送高字节
    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_TXE) == RESET);//等待发送缓冲区为空
    SPI_I2S_SendData(SPI2, dat & 0xFF);    // 发送低字节
    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);//等待发送完成
}

/*********************************************************************
 * @fn      LCD_WriteReg
 * 
 * @brief   Write a data to the register of the LCD screen  
 * @brief   向液晶显示屏寄存器写入数据
 * 
 * @param   LCD_Reg - register address
 *          LCD_RegValue - the data to write
 * 
 * @return  none
 */
void LCD_WriteReg(uint8_t LCD_Reg, uint8_t LCD_RegValue)
{
    LCD_WR_REG(LCD_Reg);
    LCD_WR_DATA(LCD_RegValue);
}

/*********************************************************************
 * @fn      Lcd_WriteData_16Bit
 * 
 * @brief   write two bytes to the lcd screen
 * 
 * @param   Data - the data to write
 * 
 * @return  none
 */
void Lcd_WriteData_16Bit(uint16_t Data)
{
    //LCD_CS_CLR;
    LCD_DC_SET;
    SPI2_Write(Data >> 8);
    SPI2_Write(Data);
    //LCD_CS_SET;
}

/*********************************************************************
 * @fn      LCD_GPIOInit
 * 
 * @brief   Configuring the control gpio for the lcd screen
 * 
 * @return  none
 */
void LCD_GPIOInit(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    RCC_APB2PeriphClockCmd(LCD_LED_RCC, ENABLE);
    RCC_APB2PeriphClockCmd(LCD_DC_RCC, ENABLE);
    RCC_APB2PeriphClockCmd(LCD_RST_RCC, ENABLE);
    RCC_APB2PeriphClockCmd(LCD_CS_RCC, ENABLE);

    GPIO_InitStructure.GPIO_Pin   = LCD_DC_PIN;//DC引脚
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;//50MHz
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;//推挽输出
    GPIO_Init(LCD_DC_PORT, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin   = LCD_RST_PIN;//复位引脚
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;//50MHz
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;//推挽输出
    GPIO_Init(LCD_RST_PORT, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin   = LCD_CS_PIN;//片选引脚
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;//50MHz
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;//推挽输出
    GPIO_Init(LCD_CS_PORT, &GPIO_InitStructure);
}

/*********************************************************************
 * @fn      LCD_Backlight_PWM_Init
 *
 * @brief   初始化PB9为TIM4_CH4 PWM输出,控制LCD背光亮度
 *          TIM4时钟=72MHz, 预分频280, ARR=255 → PWM频率≈1kHz
 *          占空比分辨率: 8位 (0-255)
 *
 * @return  none
 */
void LCD_Backlight_PWM_Init(void)
{
    GPIO_InitTypeDef        GPIO_InitStructure = {0};
    TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure = {0};
    TIM_OCInitTypeDef       TIM_OCInitStructure = {0};

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM4, ENABLE);
    RCC_APB2PeriphClockCmd(LCD_LED_RCC, ENABLE);

    /*
     * 先配TIM4并启动，CCR=0 → CH4输出始终为HIGH(高阻)，
     * 再切GPIO到AF_OD，避免GPIO先切AF时TIM4未启动导致引脚瞬间拉低闪屏
     */
    // TIM4时基: 72MHz / (280+1) / (255+1) ≈ 1kHz
    TIM_TimeBaseStructure.TIM_Period        = 255;
    TIM_TimeBaseStructure.TIM_Prescaler     = 280;
    TIM_TimeBaseStructure.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseStructure.TIM_CounterMode   = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM4, &TIM_TimeBaseStructure);

    // CH4: PWM模式1, 正常屏低电平有效(低=背光ON), 测试屏高电平有效
    TIM_OCInitStructure.TIM_OCMode      = TIM_OCMode_PWM1;
    TIM_OCInitStructure.TIM_OutputState = TIM_OutputState_Enable;
#ifdef TEST_SCREEN_BACKLIGHT
    TIM_OCInitStructure.TIM_OCPolarity  = TIM_OCPolarity_High;
#else
    TIM_OCInitStructure.TIM_OCPolarity  = TIM_OCPolarity_Low;
#endif
    TIM_OCInitStructure.TIM_Pulse       = 0;    // 初始占空比0%(全关)
    TIM_OC4Init(TIM4, &TIM_OCInitStructure);

    TIM_OC4PreloadConfig(TIM4, TIM_OCPreload_Disable);
    TIM_ARRPreloadConfig(TIM4, ENABLE);
    TIM_Cmd(TIM4, ENABLE);  // TIM4先启动，CH4已输出HIGH(高阻)，再切GPIO不会闪

    // PB9 → TIM4_CH4 (复用开漏输出：LOW=拉低亮, HIGH=高阻外部上拉灭)
    GPIO_InitStructure.GPIO_Pin   = LCD_LED_PIN;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF_OD;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(LCD_LED_PORT, &GPIO_InitStructure);
}

/*********************************************************************
 * @fn      LCD_Backlight_PWM_SetDuty
 *
 * @brief   设置背光PWM占空比(低电平占比)
 * @param   duty - 占空比 0-100, 表示低电平时间百分比(背光ON时间)
 *                0=全关, 100=全亮
 * @return  none
 */
void LCD_Backlight_PWM_SetDuty(uint8_t duty)
{
    if (duty > 100) duty = 100;
    uint16_t ccr = (uint16_t)duty * 255 / 100;
    TIM_SetCompare4(TIM4, ccr);
}

/*********************************************************************
 * @fn      LCD_Backlight_PWM_Stop
 *
 * @brief   停止PWM输出,恢复PB9为GPIO推挽输出并置高(关闭背光)
 *          用于进入待机前关闭背光
 *
 * @return  none
 */
void LCD_Backlight_PWM_Stop(void)
{
    TIM_Cmd(TIM4, DISABLE);

    // 恢复PB9为GPIO推挽输出并置高(关闭背光)
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    RCC_APB2PeriphClockCmd(LCD_LED_RCC, ENABLE);
    LCD_LED_SET;  // 切换为GPIO输出前先将ODR置高，避免背光闪亮
    GPIO_InitStructure.GPIO_Pin   = LCD_LED_PIN;
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_OD;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(LCD_LED_PORT, &GPIO_InitStructure);
}

/*********************************************************************
 * @fn      LCD_RESET
 * 
 * @brief   reset the lcd screen by the rst pin
 * 
 * @return  none
 */
void LCD_RESET(void)
{
    LCD_RST_CLR;
    Delay_Ms(100);
    LCD_RST_SET;
    Delay_Ms(50);
}

void TFT_init(void)	//GC9A01
{
    SPI2_Init();//初始化SPI2
    SPI2_DMA_Init();//初始化SPI2 DMA
    LCD_GPIOInit();//初始化LCD GPIO

	LCD_CS_CLR;//片选有效状态
	LCD_RST_CLR;//复位拉低
	Delay_Ms(20);
	LCD_RST_SET;//复位拉高，正常工作
	Delay_Ms(20);

	LCD_CS_SET;
	LCD_CS_CLR;

		
	LCD_WR_REG(0xFE);			 
	LCD_WR_REG(0xEF); 
	 
	LCD_WR_REG(0x80);
	LCD_WR_DATA(0xFF);
		
	LCD_WR_REG(0x81);
	LCD_WR_DATA(0xFF);

	LCD_WR_REG(0x82);
	LCD_WR_DATA(0xFF);

	LCD_WR_REG(0x83);
	LCD_WR_DATA(0xFF);

	LCD_WR_REG(0x84);			
	LCD_WR_DATA(0xFF); 

	LCD_WR_REG(0x85);
	LCD_WR_DATA(0xFF); 

	LCD_WR_REG(0x86);            
	LCD_WR_DATA(0xFF); 

	LCD_WR_REG(0x87);            
	LCD_WR_DATA(0xFF);

	LCD_WR_REG(0x88);            
	LCD_WR_DATA(0xFF);

	LCD_WR_REG(0x89);            
	LCD_WR_DATA(0xFF);

	LCD_WR_REG(0x8A);            
	LCD_WR_DATA(0xFF);

	LCD_WR_REG(0x8B);            
	LCD_WR_DATA(0xFF);

	LCD_WR_REG(0x8C);            
	LCD_WR_DATA(0xFF);

	LCD_WR_REG(0x8D);            
	LCD_WR_DATA(0xFF);

	LCD_WR_REG(0x8E);        
	LCD_WR_DATA(0xFF); 

	LCD_WR_REG(0x8F);     
	LCD_WR_DATA(0xFF); 

	LCD_WR_REG(0x3A);     
	LCD_WR_DATA(0x05);

	LCD_WR_REG(0xEC);     
	LCD_WR_DATA(0x01);

	LCD_WR_REG(0x74);		
	LCD_WR_DATA(0x02);
	LCD_WR_DATA(0x0E);
	LCD_WR_DATA(0x00);
	LCD_WR_DATA(0x00);
	LCD_WR_DATA(0x00);
	LCD_WR_DATA(0x00);
	LCD_WR_DATA(0x00);

	LCD_WR_REG(0x98);     
	LCD_WR_DATA(0x3E);
	LCD_WR_REG(0x99);     
	LCD_WR_DATA(0x3E);

	LCD_WR_REG(0xB5);			
	LCD_WR_DATA(0x0D);
	LCD_WR_DATA(0x0D);

	LCD_WR_REG(0x60);	
	LCD_WR_DATA(0x38);	
	LCD_WR_DATA(0x0F);
	LCD_WR_DATA(0x79);
	LCD_WR_DATA(0x67);

	LCD_WR_REG(0x61);	
	LCD_WR_DATA(0x38);
	LCD_WR_DATA(0x11);
	LCD_WR_DATA(0x79);
	LCD_WR_DATA(0x67);

	LCD_WR_REG(0x64);		
	LCD_WR_DATA(0x38);
	LCD_WR_DATA(0x17);
	LCD_WR_DATA(0x71);
	LCD_WR_DATA(0x5F);
	LCD_WR_DATA(0x79);
	LCD_WR_DATA(0x67);

	LCD_WR_REG(0x65);		
	LCD_WR_DATA(0x38);
	LCD_WR_DATA(0x13);
	LCD_WR_DATA(0x71);
	LCD_WR_DATA(0x5B);
	LCD_WR_DATA(0x79);
	LCD_WR_DATA(0x67);

	LCD_WR_REG(0x6A);		
	LCD_WR_DATA(0x00);
	LCD_WR_DATA(0x00);

	LCD_WR_REG(0x6C);			
	LCD_WR_DATA(0x22);
	LCD_WR_DATA(0x02);
	LCD_WR_DATA(0x22);
	LCD_WR_DATA(0x02);
	LCD_WR_DATA(0x22);
	LCD_WR_DATA(0x22);
	LCD_WR_DATA(0x50);

	LCD_WR_REG(0x6E);
	LCD_WR_DATA(0x03);
	LCD_WR_DATA(0x03);
	LCD_WR_DATA(0x01);
	LCD_WR_DATA(0x01);
	LCD_WR_DATA(0x00);
	LCD_WR_DATA(0x00);
	LCD_WR_DATA(0x0f);
	LCD_WR_DATA(0x0f);
	LCD_WR_DATA(0x0d);
	LCD_WR_DATA(0x0d);
	LCD_WR_DATA(0x0b);
	LCD_WR_DATA(0x0b);
	LCD_WR_DATA(0x09);
	LCD_WR_DATA(0x09);
	LCD_WR_DATA(0x00);
	LCD_WR_DATA(0x00);
	LCD_WR_DATA(0x00);
	LCD_WR_DATA(0x00);
	LCD_WR_DATA(0x0a);//
	LCD_WR_DATA(0x0a);//
	LCD_WR_DATA(0x0c);
	LCD_WR_DATA(0x0c);
	LCD_WR_DATA(0x0e);
	LCD_WR_DATA(0x0e);
	LCD_WR_DATA(0x10);
	LCD_WR_DATA(0x10);
	LCD_WR_DATA(0x00);
	LCD_WR_DATA(0x00);
	LCD_WR_DATA(0x02);
	LCD_WR_DATA(0x02);
	LCD_WR_DATA(0x04);
	LCD_WR_DATA(0x04);

	LCD_WR_REG(0xbf);			
	LCD_WR_DATA(0x01);

	LCD_WR_REG(0xF9);			
	LCD_WR_DATA(0x40);

	LCD_WR_REG(0x9b);			
	LCD_WR_DATA(0x3b);
	LCD_WR_REG(0x93);
	LCD_WR_DATA(0x33);
	LCD_WR_DATA(0x7f);
	LCD_WR_DATA(0x00);

	LCD_WR_REG(0x7E);     
	LCD_WR_DATA(0x30);

	LCD_WR_REG(0x70);
	LCD_WR_DATA(0x0d);
	LCD_WR_DATA(0x02);
	LCD_WR_DATA(0x08);
	LCD_WR_DATA(0x0d);
	LCD_WR_DATA(0x02);
	LCD_WR_DATA(0x08);

	LCD_WR_REG(0x71);
	LCD_WR_DATA(0x0d);
	LCD_WR_DATA(0x02);
	LCD_WR_DATA(0x08);

	LCD_WR_REG(0x91);
	LCD_WR_DATA(0x0E);
	LCD_WR_DATA(0x09);

	LCD_WR_REG(0xc3);
	LCD_WR_DATA(0x18);
	LCD_WR_REG(0xc4);
	LCD_WR_DATA(0x18);
	LCD_WR_REG(0xc9);
	LCD_WR_DATA(0x3c);

	LCD_WR_REG(0xf0);
	LCD_WR_DATA(0x13);
	LCD_WR_DATA(0x15);
	LCD_WR_DATA(0x04);
	LCD_WR_DATA(0x05);
	LCD_WR_DATA(0x01);
	LCD_WR_DATA(0x38);

	LCD_WR_REG(0xf2);
	LCD_WR_DATA(0x13);
	LCD_WR_DATA(0x15);
	LCD_WR_DATA(0x04);
	LCD_WR_DATA(0x05);
	LCD_WR_DATA(0x01);
	LCD_WR_DATA(0x34);

	LCD_WR_REG(0xf1);
	LCD_WR_DATA(0x4b);
	LCD_WR_DATA(0xb8);
	LCD_WR_DATA(0x7b);
	LCD_WR_DATA(0x34);
	LCD_WR_DATA(0x35);
	LCD_WR_DATA(0xef);

	LCD_WR_REG(0xf3);
	LCD_WR_DATA(0x47);
	LCD_WR_DATA(0xb4);
	LCD_WR_DATA(0x72);
	LCD_WR_DATA(0x34);
	LCD_WR_DATA(0x35);
	LCD_WR_DATA(0xda);

	LCD_WR_REG(0x36);			
	LCD_WR_DATA(0x00);

	LCD_WR_REG(0x11);
	Delay_Ms(200); 

	LCD_WR_REG(0x29);
	LCD_WR_REG(0x2C);   
	LCD_CS_SET;//片选有效状态
	Delay_Ms(100);
}

/*********************************************************************
 * @fn      LCD_Init
 * 
 * @brief   Initialization LCD screen
 * 
 * @return  none
 */
void LCD_Init(void)
{
    SPI2_Init();//初始化SPI2
    SPI2_DMA_Init();//初始化SPI2 DMA
    LCD_GPIOInit();//初始化LCD GPIO

	LCD_CS_CLR;//片选有效状态
	LCD_RST_CLR;//复位拉低
	Delay_Ms(1);
	LCD_RST_SET;//复位拉高，正常工作
	Delay_Ms(5);

	LCD_CS_SET;
	LCD_CS_CLR;
	
	LCD_WR_REG(0xEF);
	LCD_WR_REG(0xEB);
	LCD_WR_DATA(0x14); 
	
    LCD_WR_REG(0xFE);			 
	LCD_WR_REG(0xEF); 

	LCD_WR_REG(0xEB);	
	LCD_WR_DATA(0x14); 

	LCD_WR_REG(0x84);			
	LCD_WR_DATA(0x40); 

	LCD_WR_REG(0x85);			
	LCD_WR_DATA(0xFF); 

	LCD_WR_REG(0x86);			
	LCD_WR_DATA(0xFF); 

	LCD_WR_REG(0x87);			
	LCD_WR_DATA(0xFF);

	LCD_WR_REG(0x88);			
	LCD_WR_DATA(0x0A);

	LCD_WR_REG(0x89);			
	LCD_WR_DATA(0x21); 

	LCD_WR_REG(0x8A);			
	LCD_WR_DATA(0x00); 

	LCD_WR_REG(0x8B);			
	LCD_WR_DATA(0x80); 

	LCD_WR_REG(0x8C);			
	LCD_WR_DATA(0x01); 

	LCD_WR_REG(0x8D);			
	LCD_WR_DATA(0x01); 

	LCD_WR_REG(0x8E);			
	LCD_WR_DATA(0xFF); 

	LCD_WR_REG(0x8F);			
	LCD_WR_DATA(0xFF); 


	LCD_WR_REG(0xB6);
	LCD_WR_DATA(0x00);
	LCD_WR_DATA(0x20);

	LCD_WR_REG(0x36);
	if(USE_HORIZONTAL==0)LCD_WR_DATA(0x08);
	else if(USE_HORIZONTAL==1)LCD_WR_DATA(0xC8);
	else if(USE_HORIZONTAL==2)LCD_WR_DATA(0x68);
	else LCD_WR_DATA(0xA8);

	LCD_WR_REG(0x3A);			
	LCD_WR_DATA(0x05); 


	LCD_WR_REG(0x90);			
	LCD_WR_DATA(0x08);
	LCD_WR_DATA(0x08);
	LCD_WR_DATA(0x08);
	LCD_WR_DATA(0x08); 

	LCD_WR_REG(0xBD);			
	LCD_WR_DATA(0x06);
	
	LCD_WR_REG(0xBC);			
	LCD_WR_DATA(0x00);	

	LCD_WR_REG(0xFF);			
	LCD_WR_DATA(0x60);
	LCD_WR_DATA(0x01);
	LCD_WR_DATA(0x04);

	LCD_WR_REG(0xC3);			
	LCD_WR_DATA(0x13);
	LCD_WR_REG(0xC4);			
	LCD_WR_DATA(0x13);

	LCD_WR_REG(0xC9);			
	LCD_WR_DATA(0x22);

	LCD_WR_REG(0xBE);			
	LCD_WR_DATA(0x11); 

	LCD_WR_REG(0xE1);			
	LCD_WR_DATA(0x10);
	LCD_WR_DATA(0x0E);

	LCD_WR_REG(0xDF);			
	LCD_WR_DATA(0x21);
	LCD_WR_DATA(0x0c);
	LCD_WR_DATA(0x02);

	LCD_WR_REG(0xF0);   
	LCD_WR_DATA(0x45);
	LCD_WR_DATA(0x09);
	LCD_WR_DATA(0x08);
	LCD_WR_DATA(0x08);
	LCD_WR_DATA(0x26);
 	LCD_WR_DATA(0x2A);

 	LCD_WR_REG(0xF1);    
 	LCD_WR_DATA(0x43);
 	LCD_WR_DATA(0x70);
 	LCD_WR_DATA(0x72);
 	LCD_WR_DATA(0x36);
 	LCD_WR_DATA(0x37);  
 	LCD_WR_DATA(0x6F);


 	LCD_WR_REG(0xF2);   
 	LCD_WR_DATA(0x45);
 	LCD_WR_DATA(0x09);
 	LCD_WR_DATA(0x08);
 	LCD_WR_DATA(0x08);
 	LCD_WR_DATA(0x26);
 	LCD_WR_DATA(0x2A);

 	LCD_WR_REG(0xF3);   
 	LCD_WR_DATA(0x43);
 	LCD_WR_DATA(0x70);
 	LCD_WR_DATA(0x72);
 	LCD_WR_DATA(0x36);
 	LCD_WR_DATA(0x37); 
 	LCD_WR_DATA(0x6F);

	LCD_WR_REG(0xED);	
	LCD_WR_DATA(0x1B); 
	LCD_WR_DATA(0x0B); 

	LCD_WR_REG(0xAE);			
	LCD_WR_DATA(0x77);
	
	LCD_WR_REG(0xCD);			
	LCD_WR_DATA(0x63);		


	LCD_WR_REG(0x70);			
	LCD_WR_DATA(0x07);
	LCD_WR_DATA(0x07);
	LCD_WR_DATA(0x04);
	LCD_WR_DATA(0x0E); 
	LCD_WR_DATA(0x0F); 
	LCD_WR_DATA(0x09);
	LCD_WR_DATA(0x07);
	LCD_WR_DATA(0x08);
	LCD_WR_DATA(0x03);

	LCD_WR_REG(0xE8);			
	LCD_WR_DATA(0x34);

	LCD_WR_REG(0x62);			
	LCD_WR_DATA(0x18);
	LCD_WR_DATA(0x0D);
	LCD_WR_DATA(0x71);
	LCD_WR_DATA(0xED);
	LCD_WR_DATA(0x70); 
	LCD_WR_DATA(0x70);
	LCD_WR_DATA(0x18);
	LCD_WR_DATA(0x0F);
	LCD_WR_DATA(0x71);
	LCD_WR_DATA(0xEF);
	LCD_WR_DATA(0x70); 
	LCD_WR_DATA(0x70);

	LCD_WR_REG(0x63);			
	LCD_WR_DATA(0x18);
	LCD_WR_DATA(0x11);
	LCD_WR_DATA(0x71);
	LCD_WR_DATA(0xF1);
	LCD_WR_DATA(0x70); 
	LCD_WR_DATA(0x70);
	LCD_WR_DATA(0x18);
	LCD_WR_DATA(0x13);
	LCD_WR_DATA(0x71);
	LCD_WR_DATA(0xF3);
	LCD_WR_DATA(0x70); 
	LCD_WR_DATA(0x70);

	LCD_WR_REG(0x64);			
	LCD_WR_DATA(0x28);
	LCD_WR_DATA(0x29);
	LCD_WR_DATA(0xF1);
	LCD_WR_DATA(0x01);
	LCD_WR_DATA(0xF1);
	LCD_WR_DATA(0x00);
	LCD_WR_DATA(0x07);

	LCD_WR_REG(0x66);			
	LCD_WR_DATA(0x3C);
	LCD_WR_DATA(0x00);
	LCD_WR_DATA(0xCD);
	LCD_WR_DATA(0x67);
	LCD_WR_DATA(0x45);
	LCD_WR_DATA(0x45);
	LCD_WR_DATA(0x10);
	LCD_WR_DATA(0x00);
	LCD_WR_DATA(0x00);
	LCD_WR_DATA(0x00);

	LCD_WR_REG(0x67);			
	LCD_WR_DATA(0x00);
	LCD_WR_DATA(0x3C);
	LCD_WR_DATA(0x00);
	LCD_WR_DATA(0x00);
	LCD_WR_DATA(0x00);
	LCD_WR_DATA(0x01);
	LCD_WR_DATA(0x54);
	LCD_WR_DATA(0x10);
	LCD_WR_DATA(0x32);
	LCD_WR_DATA(0x98);

	LCD_WR_REG(0x74);			
	LCD_WR_DATA(0x10);	
	LCD_WR_DATA(0x85);	
	LCD_WR_DATA(0x80);
	LCD_WR_DATA(0x00); 
	LCD_WR_DATA(0x00); 
	LCD_WR_DATA(0x4E);
	LCD_WR_DATA(0x00);					
	
  LCD_WR_REG(0x98);			
	LCD_WR_DATA(0x3e);
	LCD_WR_DATA(0x07);

	LCD_WR_REG(0x35);	
	LCD_WR_REG(0x21);

	LCD_WR_REG(0x11);
	Delay_Ms(120);
	LCD_WR_REG(0x29);
	LCD_CS_SET;//片选无效状态
	Delay_Ms(100);	
}

/*********************************************************************
 * @fn      LCD_SetWindows
 * 
 * @brief   Setting LCD display window
 * 
 * @param   xStar -  the bebinning x coordinate of the LCD display window
 *          yStar -  the bebinning y coordinate of the LCD display window
 *          xEnd -  the endning x coordinate of the LCD display window
 *          yEnd -  the endning y coordinate of the LCD display window
 * 
 * @return  none
 */
void LCD_SetWindows(uint16_t xStar, uint16_t yStar, uint16_t xEnd, uint16_t yEnd)
{
    LCD_WR_REG(LCD_SET_X);
    LCD_WR_DATA(xStar >> 8);
    LCD_WR_DATA(0x00FF & xStar);
    LCD_WR_DATA(xEnd >> 8);
    LCD_WR_DATA(0x00FF & xEnd);

    LCD_WR_REG(LCD_SET_Y);
    LCD_WR_DATA(yStar >> 8);
    LCD_WR_DATA(0x00FF & yStar);
    LCD_WR_DATA(yEnd >> 8);
    LCD_WR_DATA(0x00FF & yEnd);
    LCD_WR_REG(LCD_MEM_WRITE);  
}


/*********************************************************************
 * @fn      LCD_Clear
 * 
 * @brief   Full screen filled LCD screen
 * 
 * @param   Color -   Filled color
 * 
 * @return  none
 */
void LCD_Clear(uint16_t Color)
{
    uint32_t total_size = LCD_W * LCD_H;
//    uint16_t i, j;

    //LCD_LED_CLR;// 开背光
    LCD_CS_CLR;//片选有效状态
    LCD_Address_Set_DMA(0, 0, LCD_W - 1, LCD_H - 1);//设置显示范围
    LCD_DC_SET;//数据写入有效状态

    LCD_DMA_Write_Buffer(Color, total_size);

     while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);//等待发送完成
    LCD_CS_SET;//片选无效状态
}



/******************************************************************************
      函数说明：设置起始和结束地址
      入口数据：x1,x2 设置列的起始和结束地址
                y1,y2 设置行的起始和结束地址
      返回值：  无
******************************************************************************/
void LCD_Address_Set(uint16_t x1,uint16_t y1,uint16_t x2,uint16_t y2)
{
		//P_SWI = 0x00;
		LCD_WR_REG(0x2a);//列地址设置
		LCD_WR_DATA_16Bit(x1);
		LCD_WR_DATA_16Bit(x2);
		LCD_WR_REG(0x2b);//行地址设置
		LCD_WR_DATA_16Bit(y1);
		LCD_WR_DATA_16Bit(y2);
		LCD_WR_REG(0x2c);//储存器写
		//P_SWI = 0x04;
}



/******************************************************************************
      函数说明：DMA设置起始和结束地址
      入口数据：x1,x2 设置列的起始和结束地址
                y1,y2 设置行的起始和结束地址
      返回值：  无
******************************************************************************/

void LCD_Address_Set_DMA(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    uint8_t *buf = shared_buffer;
    
    buf[0] = (x1 >> 8) & 0xFF;
    buf[1] = x1 & 0xFF;
    buf[2] = (x2 >> 8) & 0xFF;
    buf[3] = x2 & 0xFF;
    
    buf[4] = (y1 >> 8) & 0xFF;
    buf[5] = y1 & 0xFF;
    buf[6] = (y2 >> 8) & 0xFF;
    buf[7] = y2 & 0xFF;
    
    // 发送列地址命令和数据
    LCD_DC_CLR;//设置DC引脚为低电平，使能命令写入
    SPI_I2S_SendData(SPI2, 0x2a & 0xFF);//发送列地址设置

    DMA_Cmd(SPI2_DMA_TX_CH, DISABLE);//禁用DMA通道
    DMA_ClearFlag(SPI2_DMA_TC_FLAG);//清除DMA中断标志位
    SPI2_DMA_TX_CH->MADDR = (uint32_t)buf;//设置DMA通道的内存地址为buf，即列地址数据
    SPI2_DMA_TX_CH->CNTR = 4;//设置DMA通道的传输数据长度为4字节
    //while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);//等待发送完成
    LCD_DC_SET;//设置DC引脚为高电平，使能数据写入
    DMA_Cmd(SPI2_DMA_TX_CH, ENABLE);//使能DMA通道
    //while (DMA_GetFlagStatus(SPI2_DMA_TC_FLAG) == RESET);  // 等待DMA完成
    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);//等待SPI2空闲

    // 发送行地址命令和数据
    LCD_DC_CLR;
    SPI_I2S_SendData(SPI2, 0x2b & 0xFF);//发送行地址设置

    DMA_Cmd(SPI2_DMA_TX_CH, DISABLE);//禁用DMA通道
    DMA_ClearFlag(SPI2_DMA_TC_FLAG);//清除DMA中断标志位
    SPI2_DMA_TX_CH->MADDR = (uint32_t)(buf + 4);//设置DMA通道的内存地址为buf+4，即行地址数据
    SPI2_DMA_TX_CH->CNTR = 4;//设置DMA通道的传输数据长度为4字节
   // while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);//等待发送完成
    LCD_DC_SET;//设置DC引脚为高电平，使能数据写入
    DMA_Cmd(SPI2_DMA_TX_CH, ENABLE);//使能DMA通道
    //while (DMA_GetFlagStatus(SPI2_DMA_TC_FLAG) == RESET);  // 等待DMA完成
    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);//等待SPI2空闲

    // 发送写命令
    LCD_DC_CLR;
    SPI_I2S_SendData(SPI2, 0x2c & 0xFF);//发送写命令
    //while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);//等待发送完成
}



/******************************************************************************
      函数说明：DMA设置起始和结束地址,用计算DMA发送剩余数量的方式切换DC
      入口数据：x1,x2 设置列的起始和结束地址
                y1,y2 设置行的起始和结束地址
      返回值：  无
******************************************************************************/

void LCD_Address_Set_DMA_Calculate_remaining(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2)
{
    uint8_t buf[11] = {0};
    // uint16_t remaining = 0;
    
    buf[0] = 0x2a;
    buf[1] = (x1 >> 8) & 0xFF;
    buf[2] = x1 & 0xFF;
    buf[3] = (x2 >> 8) & 0xFF;
    buf[4] = x2 & 0xFF;
    buf[5] = 0x2b;
    buf[6] = (y1 >> 8) & 0xFF;
    buf[7] = y1 & 0xFF;
    buf[8] = (y2 >> 8) & 0xFF;
    buf[9] = y2 & 0xFF;
    buf[10] = 0x2c;

    // 发送列地址命令和数据
    LCD_CS_CLR;  // ? 选中LCD
    LCD_DC_CLR;//设置DC引脚为低电平，使能命令写入

    DMA_Cmd(SPI2_DMA_TX_CH, DISABLE);//禁用DMA通道
    DMA_ClearFlag(SPI2_DMA_TC_FLAG);//清除DMA中断标志位
    SPI2_DMA_TX_CH->MADDR = (uint32_t)buf;//设置DMA通道的内存地址为buf，即列地址数据
    SPI2_DMA_TX_CH->CNTR = 11;//设置DMA通道的传输数据长度为11字节
    DMA_Cmd(SPI2_DMA_TX_CH, ENABLE);//使能DMA通道


    while ((SPI2_DMA_TX_CH->CNTR) >= 11);
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    LCD_DC_SET;//设置DC引脚为高电平，使能数据写入
    while ((SPI2_DMA_TX_CH->CNTR) >= 7);
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    LCD_DC_CLR;//设置DC引脚为低电平，使能命令写入
    while ((SPI2_DMA_TX_CH->CNTR) >= 6);
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    LCD_DC_SET;//设置DC引脚为高电平，使能命令写入
    while ((SPI2_DMA_TX_CH->CNTR) >= 2);
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();
    __NOP();

    LCD_DC_CLR;//设置DC引脚为低电平，使能数据写入

    while (DMA_GetFlagStatus(SPI2_DMA_TC_FLAG) == RESET);  // 等待DMA完成
    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);//等待SPI2空闲
}

/******************************************************************************
      函数说明：DMA中断初始化
      入口数据：无
      返回值：  无
******************************************************************************/
void LCD_DMA_Interrupt_Init(void)
{
    NVIC_InitTypeDef NVIC_InitStructure={0};
    
    NVIC_InitStructure.NVIC_IRQChannel = DMA1_Channel5_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
    
    DMA_ClearFlag(DMA1_FLAG_TC5);
    DMA_ITConfig(DMA1_Channel5, DMA_IT_TC, DISABLE);
}




/*********************************************************************
 * @fn      LCD_drawImageWithSize
 * 
 * @brief   从SPI Flash读取图像并显示在LCD屏幕上
 * 
 * @param   x - LCD屏幕的X坐标
 *          y - LCD屏幕的Y坐标
 *          width - 图像宽度
 *          height - 图像高度
 *          addr - SPI Flash中的图像地址
 * 
 * @return  无
 */
// void LCD_drawImageWithSize(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint32_t addr)
// {
//     uint32_t m  = 0;
//     uint16_t x2 = (x + width) - 1;
//     uint16_t y2 = (y + height) - 1;
//     LCD_WR_REG(LCD_SET_X);
//     LCD_WR_DATA(x >> 8);
//     LCD_WR_DATA(0x00FF & x);
//     LCD_WR_DATA(x2 >> 8);
//     LCD_WR_DATA(0x00FF & x2);

//     LCD_WR_REG(LCD_SET_Y);
//     LCD_WR_DATA(y >> 8);
//     LCD_WR_DATA(0x00FF & y);
//     LCD_WR_DATA(y2 >> 8);
//     LCD_WR_DATA(0x00FF & y2);

//     LCD_WR_REG(LCD_MEM_WRITE);
//     LCD_CS_CLR;
//     LCD_DC_SET;

//     uint32_t count = width * height;

//     uint32_t fill_count = count / FILL_BUFFER_COUNT;
//     uint32_t else_count = count % FILL_BUFFER_COUNT;
//     SPI_Flash_Read_dma_start(addr);

//     SPI1_Read_DMA((uint8_t *)color_buffer[m], FILL_BUFFER_SIZE);

//     while (fill_count > 0)
//     {
//         while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET)
//             ;
//         fill_count--;
//         SPI2_Write_DMA((uint8_t *)color_buffer[m], FILL_BUFFER_SIZE);//写入DMA缓冲区数据
//         m = !m;
//         SPI1_Read_DMA((uint8_t *)color_buffer[m], FILL_BUFFER_SIZE);//读取DMA缓冲区数据
//     }
//     while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET)
//         ;
//     SPI2_Write_DMA((uint8_t *)color_buffer[m], else_count * COLOR_BYTE);
//     SPI_Flash_Read_dma_end();
// }


/******************************************************************************
      函数说明：在指定区域填充颜色
      入口数据：xsta,ysta   起始坐标
                xend,yend   终止坐标
								color       要填充的颜色
      返回值：  无
******************************************************************************/
 void LCD_Fill(uint16_t xsta,uint16_t ysta,uint16_t xend,uint16_t yend,uint16_t color)
 {          
 	uint16_t i,j;
 	//LCD_LED_CLR;//打开背光
	LCD_CS_CLR;//片选有效状态
	LCD_Address_Set(xsta,ysta,xend-1,yend-1);//设置显示范围
		
 	for(i=ysta;i<yend;i++)
 	{													   	 	
 		for(j=xsta;j<xend;j++)
 		{
 			LCD_WR_DATA_16Bit(color);
 		}
 	} 	
 	LCD_CS_SET;//片选无效状态
 }




/******************************************************************************
      函数说明：DMA填充指定区域颜色
      入口数据：xsta,ysta   起始坐标
                xend,yend   终止坐标
								color       要填充的颜色
      返回值：  无
******************************************************************************/
void LCD_Fill_DMA(uint16_t xsta, uint16_t ysta, uint16_t xend, uint16_t yend, uint16_t color)
{
    uint32_t total_size = (xend - xsta) * (yend - ysta);

    //LCD_LED_CLR;// 开背光
    LCD_CS_CLR;//片选有效状态
    LCD_Address_Set_DMA(xsta, ysta, xend - 1, yend - 1);//设置显示范围
    LCD_DC_SET;//数据写入有效状态


    // 调用简化的DMA写入函数
    //LCD_DMA_Write_Buffer_Add_coordinates(color>>8,color, total_size);
    LCD_DMA_Write_Buffer(color, total_size);

    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);//等待发送完成
    LCD_CS_SET;//片选无效状态
}



/******************************************************************************
      函数说明：DMA写入LCD颜色缓冲区
      入口数据：color       要填充的颜色
                total_size  要填充的像素数
      返回值：  无
******************************************************************************/
void LCD_DMA_Write_Buffer(uint16_t color, uint32_t total_size)
{
    uint8_t color_bytes[2];
    total_size = total_size * 2;
    uint32_t full_count = (total_size) / FILL_BUFFER_SIZE;
    uint16_t remain_size = (total_size) % FILL_BUFFER_SIZE;
	uint32_t less_count = 0;

    color_bytes[1] = (color >> 8) & 0xFF;  // 高字节
    color_bytes[0] = color & 0xFF;         // 低字节

	while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_TXE) == RESET)//等待发送缓冲区为空
		;

    //填充缓冲区（重复 pattern）
	FastFill_16bit_Repeat(shared_buffer, total_size, color_bytes[1], color_bytes[0]);

    // 发送完整缓冲区块
    while (full_count > 0) {
        while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);//等待上一次传输完成
        full_count--;
        SPI2_Write_DMA(shared_buffer, FILL_BUFFER_SIZE);
	}

	
    // 发送剩余数据
    if (remain_size > 0)
    {
		less_count = total_size < FILL_BUFFER_SIZE ? total_size : remain_size;
        while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET)
        ;
        SPI2_Write_DMA(shared_buffer, less_count);
    }
    
    // 等待最后一次传输完成
    //while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);
}

/*********************************************************************
 * @fn      LCD_Enter_Sleep
 *
 * @brief   Enter sleep mode (power down LCD controller)
 *
 * @return  none
 */
void LCD_Enter_Sleep(void)
{
    LCD_CS_CLR;
    LCD_WR_REG(0x28);   // DISPLAY_OFF
    LCD_WR_REG(0x10);   // ENTER_SLEEP_MODE
    LCD_CS_SET;
    Delay_Ms(10);
}

/*********************************************************************
 * @fn      LCD_Exit_Sleep
 *
 * @brief   Exit sleep mode (wake up LCD controller)
 *
 * @return  none
 */
void LCD_Exit_Sleep(void)
{
    LCD_CS_CLR;
    LCD_WR_REG(0x11);   // SLEEP_OUT_MODE
    LCD_CS_SET;
    Delay_Ms(120);
    LCD_CS_CLR;
    LCD_WR_REG(0x29);   // DISPLAY_ON
    LCD_CS_SET;
}


/******************************************************************************
      函数说明：DMA写入LCD颜色缓冲区
      入口数据：color       要填充的颜色
                total_size  要填充的像素数
      返回值：  无
******************************************************************************/
#define DMA_PREFILL_COUNT    8     // DMA预填充字节数：先填前8字节就启动DMA，剩余边发边填
void LCD_DMA_Write_Buffer_Add_coordinates(uint16_t datah, uint16_t datal, uint32_t total_size)
{
    uint32_t total_sizes = total_size * 2;//像素总数
    uint32_t full_count = total_sizes / FILL_BUFFER_COUNT;//完整块数
    uint16_t remain_size = total_sizes % FILL_BUFFER_COUNT;//剩余像素数
    uint32_t T;

    LCD_DC_SET;

    if (full_count > 0) {
        // 预填充前 DMA_PREFILL_COUNT 字节，给 DMA 一个"提前量"
        for (T = 0; T < DMA_PREFILL_COUNT; T += 2) {
            shared_buffer[T] = datah;
            shared_buffer[T + 1] = datal;
        }

        SPI2_WaitIdle();//等待SPI2空闲

        // 立即启动 DMA（此时前64字节已就绪，DMA从offset 0开始读取）
        SPI2_StartDMA((uint8_t*)shared_buffer, FILL_BUFFER_COUNT);
        full_count--;

        // DMA发送前64字节期间，CPU继续填充剩余字节（CPU写SRAM远快于SPI DMA读）
        for (T = DMA_PREFILL_COUNT; T < FILL_BUFFER_COUNT; T += 2) {
            shared_buffer[T] = datah;
            shared_buffer[T + 1] = datal;
        }

        // 发送剩余完整块（缓冲区已全部填好）
        while (full_count > 0) {
            while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);
            full_count--;
            SPI2_StartDMA((uint8_t*)shared_buffer, FILL_BUFFER_COUNT);
        }
    } else {
        SPI2_WaitIdle();
    }

    // 发送剩余数据
    if (remain_size > 0) {
        if (total_sizes < FILL_BUFFER_COUNT) {//像素总数小于缓冲区大小，无完整块
            // 无完整块，单独处理剩余数据
            if (remain_size > DMA_PREFILL_COUNT) {
                // 剩余数据也够大，同样预填充后启动DMA，边发边填
                for (T = 0; T < DMA_PREFILL_COUNT; T += 2) {
                    shared_buffer[T] = datah;
                    shared_buffer[T + 1] = datal;
                }
                // SPI2_WaitIdle() 已在上面 else 分支调用过，直接启动DMA
                SPI2_StartDMA((uint8_t*)shared_buffer, remain_size);
                for (T = DMA_PREFILL_COUNT; T < remain_size; T += 2) {
                    shared_buffer[T] = datah;
                    shared_buffer[T + 1] = datal;
                }
            } else {
                // 数据量小，直接全填再发
                for (T = 0; T < remain_size; T += 2) {
                    shared_buffer[T] = datah;
                    shared_buffer[T + 1] = datal;
                }
                while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);
                SPI2_StartDMA((uint8_t*)shared_buffer, remain_size);
            }
        } else {
            // full_count > 0 的情况，缓冲区已在上面填好，直接发送
            while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);
            SPI2_StartDMA((uint8_t*)shared_buffer, remain_size);
        }
    }
}


/*********************************************************************
 * @fn      LCD_drawImageWithSize
 * 
 * @brief   从SPI Flash读取图像并显示在LCD屏幕上
 * 
 * @param   count - 图像像素数量
 *          addr - SPI Flash中的图像地址
 * 
 * @return  无
 */
uint32_t byte_count;
uint32_t block_size;
uint32_t fill_count;
uint32_t length;
uint32_t front_count;
uint32_t front_read_count;
uint32_t remain_count;
uint32_t remain_read_count;
uint32_t current_count;
uint32_t current_count1;
uint32_t current_read_count;


void LCD_drawImageWithSize_DMA(uint16_t count, uint8_t pre_count) {
    byte_count = (uint32_t)count * 2;// //总数<<1为*2
    block_size = PARALLEL_BUFFER_COUNT;
    fill_count = byte_count / block_size;//完整块数
    uint32_t else_count = byte_count % block_size;//剩余像素数
    length = byte_count;
     front_count = byte_count;//前面的数
    front_read_count = 0;//前面读取的数量
    remain_count = 0;// //剩下的数
    remain_read_count = 0; //剩下读取的数量
    current_count = 0;//当前发送的数
    current_count1 = 0;//当前发送的数
    current_read_count = 0;//当前读取的数量
    //uint32_t G = 0;


    // 等待 SPI1 就绪
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET);//等待flash读取完成
    while ((SPI2_DMA_TX_CH->CNTR) > 0);//等待SPI2屏幕刷新完成
    //while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);//等待SPI2屏幕发送完成

    // 情况1：总长度小于一个缓冲区
        if (else_count > 0) {
            SPI1_Read_DMA_NonBlocking((uint8_t*)shared_buffer + (pre_count * 2), byte_count);//非阻塞读取所有数据
            //SPI2_WaitIdle();//配置DMA传输
            LCD_DC_SET;
            if (length > 0) {
                while ((SPI1_DMA_TX_CH->CNTR) > 3) {//由于DMA接收数据时不是同时发送数据，所以要先不用CPU发送数据，等DMA接收完成后再用DMA发送数据
                    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_TXE) == RESET);//等待发送缓冲区为空
                    SPI_I2S_SendData(SPI2, ((uint16_t*)shared_buffer)[current_count]);      // 发送低字节
                    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_TXE) == RESET);//等待发送缓冲区为空
                    SPI_I2S_SendData(SPI2, (((uint16_t*)shared_buffer)[current_count] >> 8));    // 发送高字节
                    front_count--;
                    front_count--;
                    current_count++;
                }
                remain_count = byte_count - front_count;//偏移量
                SPI2_StartDMA((uint8_t*)shared_buffer + remain_count, front_count + (pre_count * 2));//启动DMA传输
                //SPI2_Write_DMA((uint8_t*)shared_buffer + remain_count, front_count + pre_count);//写入剩余数据到LCD屏幕
                SPI1_DMA_WaitComplete();          // 等待当前块读取完成
                //while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);//等待SPI2发送完成
            }
        }
        else {//数据已经被预读完
            LCD_DC_SET;
            if ((pre_count * 2) > 5) {//数据大于5个则DMA传输
                SPI2_StartDMA((uint8_t*)shared_buffer, (pre_count * 2));//启动DMA传输
            }
            else {//数据小于等于5个则直接发送
                for (uint8_t i = 0; i < pre_count; i++) {
                    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_TXE) == RESET);//等待发送缓冲区为空
                    SPI_I2S_SendData(SPI2, ((uint16_t*)shared_buffer)[i]);      // 发送低字节
                    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_TXE) == RESET);//等待发送缓冲区为空
                    SPI_I2S_SendData(SPI2, (((uint16_t*)shared_buffer)[i] >> 8));    // 发送高字节
                }
            }
        }
    // //情况2：至少一个完整块
    // else {
    //     LCD_DC_SET;
    //     while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_TXE) == RESET);//等待发送缓冲区为空
    //     SPI_I2S_SendData(SPI2, data_H & 0xFF);  // 确保只发送低8位
    //     while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_TXE) == RESET);//等待发送缓冲区为空
    //     SPI_I2S_SendData(SPI2, data_L & 0xFF);  // 确保只发送低8位

    //     // 预读第一块
    //     SPI1_Read_DMA_NonBlocking((uint8_t*)color_buffer[m], block_size);//读取第一块数据

    //     while (fill_count > 0) {
    //         SPI1_DMA_WaitComplete();          // 等待当前块读取完成
    //         while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);//等待SPI2发送完成
    //         SPI2_Write_DMA((uint8_t*)color_buffer[m], block_size);//写入当前块数据
    //         fill_count--;
    //         if (fill_count > 0) {
    //             m ^= 1;
    //             SPI1_Read_DMA_NonBlocking((uint8_t*)color_buffer[m], block_size);//预读下一块数据
    //         }
    //     }

    //     // 处理剩余数据
    //     if (else_count > 0) {
    //         SPI1_Read_DMA((uint8_t*)color_buffer[m], else_count);//读取剩余数据
    //         while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);//等待SPI2发送完成
    //         SPI2_Write_DMA((uint8_t*)color_buffer[m], else_count);//写入剩余数据
    //     }

    //     // 等待 SPI2 传输完成
    //     //while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);//等待发送完成
    // }
}

/**
 * @brief  高效填充重复的16位颜色到缓冲区（自动适应长度）
 * @param  buf     : 目标缓冲区
 * @param  len     : 填充字节数（必须偶数）
 * @param  color_l : 颜色低字节
 * @param  color_h : 颜色高字节
 */
#define FASTFILL_DMA_THRESHOLD  128   // 小于此长度用 for 循环，否则用 DMA
void FastFill_16bit_Repeat(uint8_t *buf, uint32_t len, uint8_t color_l, uint8_t color_h)
{
    len = len < FILL_BUFFER_SIZE ? len : FILL_BUFFER_SIZE;// 取较小值
    
    if (len < 2) return;

    // 短数据：直接用简单的 for 循环（无额外开销）
    if (len < FASTFILL_DMA_THRESHOLD) {
        for (uint32_t i = 0; i < len; i += 2) {
            buf[i]   = color_l;
            buf[i+1] = color_h;
        }
        return;
    }

    // 长数据：用 DMA 内存到内存快速复制
    // 先写入第一个像素作为种子
    buf[0] = color_l;
    buf[1] = color_h;

    DMA_InitTypeDef dma = {0};
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);   // 确保 DMA 时钟开启

    DMA_DeInit(DMA1_Channel6);
    dma.DMA_PeripheralBaseAddr = (uint32_t)buf;
    dma.DMA_MemoryBaseAddr     = (uint32_t)(buf + 2);
    dma.DMA_DIR                = DMA_DIR_PeripheralSRC;
    dma.DMA_BufferSize         = len - 2;
    dma.DMA_PeripheralInc      = DMA_PeripheralInc_Enable;
    dma.DMA_MemoryInc          = DMA_MemoryInc_Enable;
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    dma.DMA_MemoryDataSize     = DMA_MemoryDataSize_Byte;
    dma.DMA_Mode               = DMA_Mode_Normal;
    dma.DMA_Priority           = DMA_Priority_High;
    dma.DMA_M2M                = DMA_M2M_Enable;

    DMA_Init(DMA1_Channel6, &dma);
    DMA_ClearFlag(DMA1_FLAG_TC6);
    DMA_Cmd(DMA1_Channel6, ENABLE);

    while (DMA_GetFlagStatus(DMA1_FLAG_TC6) == RESET);
    DMA_Cmd(DMA1_Channel6, DISABLE);
}
