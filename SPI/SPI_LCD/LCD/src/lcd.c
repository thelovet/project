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
#define LCD_USE_TFT_INIT  0  // 1:启用旧版TFT_init初始化 0:禁用(节省Flash)
u8 shared_buffer[FILL_BUFFER_SIZE] = { 0 };


void FastFill_16bit_Repeat(uint8_t *buf, uint32_t len, uint8_t color_l, uint8_t color_h);//高效填充重复的16位颜色到缓冲区（自动适应长度）
void LCD_drawImageWithSize_DMA(uint16_t count,uint8_t pre_count);//从flash读取数据发送到LCD屏幕
void LCD_DMA_Write_Buffer(uint16_t color, uint32_t total_size);//将缓冲区数据发送到LCD屏幕
static void LCD_Init_CmdTbl(const uint8_t *tbl);//遍历初始化序列表，依次发送寄存器命令与数据


/**
 * @brief  向LCD写入8位命令字
 * @param  data  要写入的命令字
 * @return 无
 */
void LCD_WR_REG(uint8_t data)
{
    //LCD_CS_CLR;
    LCD_DC_CLR;
    SPI2_Write(data);
    //LCD_CS_SET;
}

/**
 * @brief  向LCD写入8位数据
 * @param  data  要写入的数据
 * @return 无
 */
void LCD_WR_DATA(uint8_t data)
{
    //LCD_CS_CLR;
    LCD_DC_SET;
    SPI2_Write(data);
    //LCD_CS_SET;
}


/**
 * @brief  向LCD写入16位数据
 * @param  dat  要写入的16位数据
 * @return 无
 */
void LCD_WR_DATA_16Bit(uint16_t dat)
{
	LCD_DC_SET;
    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_TXE) == RESET);//等待发送缓冲区为空
    SPI_I2S_SendData(SPI2, dat >> 8);      // 发送高字节
    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_TXE) == RESET);//等待发送缓冲区为空
    SPI_I2S_SendData(SPI2, dat & 0xFF);    // 发送低字节
    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);//等待发送完成
}

/**
 * @brief  向LCD寄存器写入值
 * @param  LCD_Reg       寄存器地址
 * @param  LCD_RegValue  要写入的值
 * @return 无
 */
void LCD_WriteReg(uint8_t LCD_Reg, uint8_t LCD_RegValue)
{
    LCD_WR_REG(LCD_Reg);
    LCD_WR_DATA(LCD_RegValue);
}

/**
 * @brief  向LCD写入16位数据
 * @param  Data  要写入的16位数据
 * @return 无
 */
void Lcd_WriteData_16Bit(uint16_t Data)
{
    //LCD_CS_CLR;
    LCD_DC_SET;
    SPI2_Write(Data >> 8);
    SPI2_Write(Data);
    //LCD_CS_SET;
}

/**
 * @brief  配置LCD控制引脚(DC/RST/CS)为推挽输出
 * @return 无
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

/**
 * @brief  初始化PB9为TIM4_CH4 PWM输出,控制LCD背光亮度
 *         TIM4时钟=72MHz, 预分频280, ARR=255 → PWM频率≈1kHz
 *         占空比分辨率: 8位 (0-255)
 * @return 无
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

/**
 * @brief  设置背光PWM占空比(低电平占比)
 * @param  duty  占空比0-100, 表示低电平时间百分比(背光ON时间), 0=全关,100=全亮
 * @return 无
 */
void LCD_Backlight_PWM_SetDuty(uint8_t duty)
{
    if (duty > 100) duty = 100;
    uint16_t ccr = (uint16_t)duty * 255 / 100;
    TIM_SetCompare4(TIM4, ccr);
}

/**
 * @brief  停止PWM输出,恢复PB9为GPIO推挽输出并置高(关闭背光)
 *         用于进入待机前关闭背光
 * @return 无
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

/**
 * @brief  通过RST引脚复位LCD
 * @return 无
 */
void LCD_RESET(void)
{
    LCD_RST_CLR;
    Delay_Ms(100);
    LCD_RST_SET;
    Delay_Ms(50);
}

/**
 * @brief  旧版TFT初始化序列 (GC9A01) - 当前未使用
 * @note   由 LCD_USE_TFT_INIT 宏控制是否编译，默认0禁用以节省Flash
 * @return 无
 */
#if LCD_USE_TFT_INIT
static const uint8_t tft_init_tbl[] = {
    0xFE, 0x00, 0xEF, 0x00, 0x80, 0x01, 0xFF, 0x81, 0x01, 0xFF, 0x82, 0x01,
    0xFF, 0x83, 0x01, 0xFF, 0x84, 0x01, 0xFF, 0x85, 0x01, 0xFF, 0x86, 0x01,
    0xFF, 0x87, 0x01, 0xFF, 0x88, 0x01, 0xFF, 0x89, 0x01, 0xFF, 0x8A, 0x01,
    0xFF, 0x8B, 0x01, 0xFF, 0x8C, 0x01, 0xFF, 0x8D, 0x01, 0xFF, 0x8E, 0x01,
    0xFF, 0x8F, 0x01, 0xFF, 0x3A, 0x01, 0x05, 0xEC, 0x01, 0x01, 0x74, 0x07,
    0x02, 0x0E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x98, 0x01, 0x3E, 0x99, 0x01,
    0x3E, 0xB5, 0x02, 0x0D, 0x0D, 0x60, 0x04, 0x38, 0x0F, 0x79, 0x67, 0x61,
    0x04, 0x38, 0x11, 0x79, 0x67, 0x64, 0x06, 0x38, 0x17, 0x71, 0x5F, 0x79,
    0x67, 0x65, 0x06, 0x38, 0x13, 0x71, 0x5B, 0x79, 0x67, 0x6A, 0x02, 0x00,
    0x00, 0x6C, 0x07, 0x22, 0x02, 0x22, 0x02, 0x22, 0x22, 0x50, 0x6E, 0x20,
    0x03, 0x03, 0x01, 0x01, 0x00, 0x00, 0x0f, 0x0f, 0x0d, 0x0d, 0x0b, 0x0b,
    0x09, 0x09, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x0a, 0x0c, 0x0c, 0x0e, 0x0e,
    0x10, 0x10, 0x00, 0x00, 0x02, 0x02, 0x04, 0x04, 0xBF, 0x01, 0x01, 0xF9,
    0x01, 0x40, 0x9B, 0x01, 0x3b, 0x93, 0x03, 0x33, 0x7f, 0x00, 0x7E, 0x01,
    0x30, 0x70, 0x06, 0x0d, 0x02, 0x08, 0x0d, 0x02, 0x08, 0x71, 0x03, 0x0d,
    0x02, 0x08, 0x91, 0x02, 0x0E, 0x09, 0xC3, 0x01, 0x18, 0xC4, 0x01, 0x18,
    0xC9, 0x01, 0x3c, 0xF0, 0x06, 0x13, 0x15, 0x04, 0x05, 0x01, 0x38, 0xF2,
    0x06, 0x13, 0x15, 0x04, 0x05, 0x01, 0x34, 0xF1, 0x06, 0x4b, 0xb8, 0x7b,
    0x34, 0x35, 0xef, 0xF3, 0x06, 0x47, 0xb4, 0x72, 0x34, 0x35, 0xda, 0x36,
    0x01, 0x00,
    0x00, 0x00, /* 结束标记 */
};
void TFT_init(void)
{
    SPI2_Init();     // 初始化SPI2
    SPI2_DMA_Init(); // 初始化SPI2 DMA
    LCD_GPIOInit();  // 初始化LCD GPIO

    LCD_CS_CLR;      // 片选有效
    LCD_RST_CLR;     // 复位拉低
    Delay_Ms(20);
    LCD_RST_SET;     // 复位拉高，正常工作
    Delay_Ms(20);

    LCD_CS_SET;
    LCD_CS_CLR;

    /* 初始化序列 (0xFE~0x36) */
    LCD_Init_CmdTbl(tft_init_tbl);

    LCD_WR_REG(0x11);  // 退出睡眠模式
    Delay_Ms(200);
    LCD_WR_REG(0x29);  // 开启显示
    LCD_WR_REG(0x2C);  // 写单元格
    LCD_CS_SET;        // 片选无效
    Delay_Ms(100);
}
#endif /* LCD_USE_TFT_INIT */

/**
 * @brief  遍历初始化序列表，依次发送寄存器命令与数据
 * @param  tbl  初始化序列表 (每项: 寄存器, 数据个数, 数据...)
 * @note   表以 {0x00, 0x00} 作为结束标记
 * @return 无
 */
static void LCD_Init_CmdTbl(const uint8_t *tbl)
{
    while (tbl[0] != 0x00 || tbl[1] != 0x00)
    {
        uint8_t reg = tbl[0];
        uint8_t len = tbl[1];
        LCD_WR_REG(reg);
        tbl += 2;
        while (len--)
        {
            LCD_WR_DATA(*tbl++);
        }
    }
}

/* 第1段初始化序列: 0xEF~0xB6 */
static const uint8_t lcd_init_tbl_a[] = {
    0xEF, 0x00, 0xEB, 0x01, 0x14, 0xFE, 0x00, 0xEF, 0x00, 0xEB, 0x01, 0x14,
    0x84, 0x01, 0x40, 0x85, 0x01, 0xFF, 0x86, 0x01, 0xFF, 0x87, 0x01, 0xFF,
    0x88, 0x01, 0x0A, 0x89, 0x01, 0x21, 0x8A, 0x01, 0x00, 0x8B, 0x01, 0x80,
    0x8C, 0x01, 0x01, 0x8D, 0x01, 0x01, 0x8E, 0x01, 0xFF, 0x8F, 0x01, 0xFF,
    0xB6, 0x02, 0x00, 0x20,
    0x00, 0x00, /* 结束标记 */
};

/* 第2段初始化序列: 0x3A~0x21 */
static const uint8_t lcd_init_tbl_b[] = {
    0x3A, 0x01, 0x05, 0x90, 0x04, 0x08, 0x08, 0x08, 0x08, 0xBD, 0x01, 0x06,
    0xBC, 0x01, 0x00, 0xFF, 0x03, 0x60, 0x01, 0x04, 0xC3, 0x01, 0x13, 0xC4,
    0x01, 0x13, 0xC9, 0x01, 0x22, 0xBE, 0x01, 0x11, 0xE1, 0x02, 0x10, 0x0E,
    0xDF, 0x03, 0x21, 0x0c, 0x02, 0xF0, 0x06, 0x45, 0x09, 0x08, 0x08, 0x26,
    0x2A, 0xF1, 0x06, 0x43, 0x70, 0x72, 0x36, 0x37, 0x6F, 0xF2, 0x06, 0x45,
    0x09, 0x08, 0x08, 0x26, 0x2A, 0xF3, 0x06, 0x43, 0x70, 0x72, 0x36, 0x37,
    0x6F, 0xED, 0x02, 0x1B, 0x0B, 0xAE, 0x01, 0x77, 0xCD, 0x01, 0x63, 0x70,
    0x09, 0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03, 0xE8, 0x01,
    0x34, 0x62, 0x0C, 0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71,
    0xEF, 0x70, 0x70, 0x63, 0x0C, 0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18,
    0x13, 0x71, 0xF3, 0x70, 0x70, 0x64, 0x07, 0x28, 0x29, 0xF1, 0x01, 0xF1,
    0x00, 0x07, 0x66, 0x0A, 0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00,
    0x00, 0x00, 0x67, 0x0A, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10,
    0x32, 0x98, 0x74, 0x07, 0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00, 0x98,
    0x02, 0x3e, 0x07, 0x35, 0x00, 0x21, 0x00,
    0x00, 0x00, /* 结束标记 */
};

/**
 * @brief  初始化LCD屏幕 (GC9A01)
 * @note   初始化序列使用常量表 + 循环发送，节省Flash空间
 * @return 无
 */
void LCD_Init(void)
{
    SPI2_Init();     // 初始化SPI2
    SPI2_DMA_Init(); // 初始化SPI2 DMA
    LCD_GPIOInit();  // 初始化LCD GPIO

    LCD_CS_CLR;      // 片选有效
    LCD_RST_CLR;     // 复位拉低
    Delay_Ms(1);
    LCD_RST_SET;     // 复位拉高，正常工作
    Delay_Ms(5);

    LCD_CS_SET;
    LCD_CS_CLR;

    /* 第1段初始化序列 (0xEF~0xB6) */
    LCD_Init_CmdTbl(lcd_init_tbl_a);

    /* 显存访问方向控制 (由USE_HORIZONTAL决定) */
    LCD_WR_REG(0x36);
    LCD_WR_DATA(USE_HORIZONTAL == 0 ? 0x08 :
                USE_HORIZONTAL == 1 ? 0xC8 :
                USE_HORIZONTAL == 2 ? 0x68 : 0xA8);

    /* 第2段初始化序列 (0x3A~0x21) */
    LCD_Init_CmdTbl(lcd_init_tbl_b);

    LCD_WR_REG(0x11);  // 退出睡眠模式
    Delay_Ms(120);
    LCD_WR_REG(0x29);  // 开启显示
    LCD_CS_SET;        // 片选无效
    Delay_Ms(100);
}


/**
 * @brief  设置LCD显示窗口
 * @param  xStar  窗口起始X坐标
 * @param  yStar  窗口起始Y坐标
 * @param  xEnd   窗口结束X坐标
 * @param  yEnd   窗口结束Y坐标
 * @return 无
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


/**
 * @brief  全屏填充指定颜色
 * @param  Color  填充颜色
 * @return 无
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



/**
 * @brief  设置LCD起始和结束显示地址
 * @param  x1  列起始地址
 * @param  y1  行起始地址
 * @param  x2  列结束地址
 * @param  y2  行结束地址
 * @return 无
 */
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



/**
 * @brief  DMA方式设置LCD起始和结束显示地址
 * @param  x1  列起始地址
 * @param  y1  行起始地址
 * @param  x2  列结束地址
 * @param  y2  行结束地址
 * @return 无
 */
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



/**
 * @brief  DMA方式设置起始和结束地址, 用计算DMA发送剩余数量的方式切换DC
 * @param  x1  列起始地址
 * @param  y1  行起始地址
 * @param  x2  列结束地址
 * @param  y2  行结束地址
 * @return 无
 */
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

/**
 * @brief  DMA中断初始化
 * @return 无
 */
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




/**
 * @brief  在指定区域填充颜色
 * @param  xsta  起始X坐标
 * @param  ysta  起始Y坐标
 * @param  xend  终止X坐标
 * @param  yend  终止Y坐标
 * @param  color 要填充的颜色
 * @return 无
 */
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




/**
 * @brief  DMA方式在指定区域填充颜色
 * @param  xsta  起始X坐标
 * @param  ysta  起始Y坐标
 * @param  xend  终止X坐标
 * @param  yend  终止Y坐标
 * @param  color 要填充的颜色
 * @return 无
 */
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



/**
 * @brief  DMA写入LCD颜色缓冲区
 * @param  color       要填充的颜色
 * @param  total_size  要填充的像素数
 * @return 无
 */
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

/**
 * @brief  进入睡眠模式(关闭LCD控制器)
 * @return 无
 */
void LCD_Enter_Sleep(void)
{
    LCD_CS_CLR;
    LCD_WR_REG(0x28);   // DISPLAY_OFF
    LCD_WR_REG(0x10);   // ENTER_SLEEP_MODE
    LCD_CS_SET;
    Delay_Ms(10);
}

/**
 * @brief  退出睡眠模式(唤醒LCD控制器)
 * @return 无
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


/**
 * @brief  DMA写入LCD颜色缓冲区(实时切换DC, 预填充后边发边填)
 * @param  datah      颜色高字节
 * @param  datal      颜色低字节
 * @param  total_size 要填充的像素数
 * @return 无
 */
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


/**
 * @brief  从SPI Flash读取图像并显示到LCD屏幕(块状DMA方式)
 * @param  count      图像像素数量
 * @param  pre_count  已预读的像素数量
 * @return 无
 */
void LCD_drawImageWithSize_DMA(uint16_t count, uint8_t pre_count) {
    // 以下变量仅在下方被注释掉的"情况2"代码块中使用，故注释保留（定义时赋值），不参与编译
    // uint32_t fill_count = byte_count / block_size;
    // uint32_t front_read_count = 0;
    // uint32_t remain_read_count = 0;
    // uint32_t current_count1 = 0;
    // uint32_t current_read_count = 0;
    uint32_t byte_count   = (uint32_t)count * 2;// //总数<<1为*2
    uint32_t block_size   = PARALLEL_BUFFER_COUNT;
    uint32_t length       = byte_count;
    uint32_t front_count  = byte_count;//前面的数
    uint32_t remain_count = 0;// //剩下的数
    uint32_t current_count = 0;//当前发送的数
    uint32_t else_count   = byte_count % block_size;//剩余像素数
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
 * @brief  高效填充重复的16位颜色到缓冲区(自动适应长度)
 * @param  buf      目标缓冲区
 * @param  len      填充字节数(必须偶数)
 * @param  color_l  颜色低字节
 * @param  color_h  颜色高字节
 * @return 无
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

    // 复位DMA1通道6，按默认参数清零，准备复用
    DMA_DeInit(DMA1_Channel6);

    // 配置DMA为"内存到内存"模式，把颜色数据从buf[0..1]向后复制扩散
    dma.DMA_PeripheralBaseAddr = (uint32_t)buf;          // 源地址：buf首地址(已写入第一个像素种子)
    dma.DMA_MemoryBaseAddr     = (uint32_t)(buf + 2);    // 目标地址：buf+2，从第2像素起续写
    dma.DMA_DIR                = DMA_DIR_PeripheralSRC;  // 方向：源为外设(内存)
    dma.DMA_BufferSize         = len - 2;                // 传输长度：减掉已写的2字节
    dma.DMA_PeripheralInc      = DMA_PeripheralInc_Enable;  // 源地址自增
    dma.DMA_MemoryInc          = DMA_MemoryInc_Enable;   // 目标地址自增
    dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;  // 源数据宽度：字节
    dma.DMA_MemoryDataSize     = DMA_MemoryDataSize_Byte;     // 目标数据宽度：字节
    dma.DMA_Mode               = DMA_Mode_Normal;        // 普通模式，传完即停
    dma.DMA_Priority           = DMA_Priority_High;      // 高优先级
    dma.DMA_M2M                = DMA_M2M_Enable;         // 开启内存到内存传输

    DMA_Init(DMA1_Channel6, &dma);      // 应用DMA配置
    DMA_ClearFlag(DMA1_FLAG_TC6);       // 清除传输完成标志
    DMA_Cmd(DMA1_Channel6, ENABLE);     // 启动DMA传输

    while (DMA_GetFlagStatus(DMA1_FLAG_TC6) == RESET); // 等待传输完成
    DMA_Cmd(DMA1_Channel6, DISABLE);    // 停止DMA
}
