#ifndef __LCD_H
#define __LCD_H
#include "spi.h"
#include "stdio.h"
#include "stdlib.h"
#include "flash.h"
#include "config.h"

// [15:11] 5bit red
//  [10:5] 6bit green
//   [4:0] 5bit blue
#define LCD_WHITE    0xFFFF//白色
#define LCD_BLACK    0x0000//黑色
#define LCD_BLUE     0x001F//蓝色
#define LCD_BRED     0XF81F//暗红
#define LCD_GRED     0XFFE0//灰绿
#define LCD_GBLUE    0X07FF//浅蓝
#define LCD_RED      0xF800//红色
#define LCD_MAGENTA  0xF81F//品红
#define LCD_GREEN    0x07E0//绿色
#define LCD_CYAN     0x7FFF//青色
#define LCD_YELLOW   0xFFE0//黄色
#define LCD_BROWN    0XBC40//棕色
#define LCD_BRRED    0XFC07//砖红
#define LCD_GRAY     0X8430//灰色

#ifdef TEST_SCREEN_BACKLIGHT
  #define LCD_LED_SET  (LCD_LED_PORT->BCR = LCD_LED_PIN)   // 测试屏：置低关闭背光
  #define LCD_LED_CLR  (LCD_LED_PORT->BSHR = LCD_LED_PIN)  // 测试屏：置高打开背光
#else
  #define LCD_LED_SET  (LCD_LED_PORT->BSHR = LCD_LED_PIN)  // 正常屏：置高关闭背光
  #define LCD_LED_CLR  (LCD_LED_PORT->BCR = LCD_LED_PIN)   // 正常屏：置低打开背光
#endif
#define LCD_CS_SET   (LCD_CS_PORT->BSHR = LCD_CS_PIN)    // LCD片选引脚置高 (片选无效)
#define LCD_DC_SET   (LCD_DC_PORT->BSHR = LCD_DC_PIN)    // LCD数据/命令引脚置高 (数据模式)
#define LCD_RST_SET  (LCD_RST_PORT->BSHR = LCD_RST_PIN)  // LCD复位引脚置高 (正常工作)
#define LCD_CS_CLR   (LCD_CS_PORT->BCR = LCD_CS_PIN)     // LCD片选引脚置低 (片选有效)
#define LCD_DC_CLR   (LCD_DC_PORT->BCR = LCD_DC_PIN)     // LCD数据/命令引脚置低 (命令模式)
#define LCD_RST_CLR  (LCD_RST_PORT->BCR = LCD_RST_PIN)   // LCD复位引脚置低 (复位状态)

// LCD电源控制：PB4低电平=打开供电
#define LCD_PWR_ON   (LCD_PWR_PORT->BCR = LCD_PWR_PIN)   // PB4置低→LCD上电
#ifdef KEEP_LCD_POWER_IN_STANDBY
  #define LCD_PWR_OFF  ((void)0)                           // 休眠时保持供电
#else
  #define LCD_PWR_OFF  (LCD_PWR_PORT->BSHR = LCD_PWR_PIN)  // PB4置高→LCD断电
#endif

// 一级命令
#define READ_DISPLAY_IDENTIFICATION_INFORMATION 0x04 // 读取显示识别信息
#define READ_DISPLAY_STATUS 0x09 // 读取显示状态
#define ENTER_SLEEP_MODE 0x10 // 进入睡眠模式
#define SLEEP_OUT_MODE 0x11 // 睡眠退出模式
#define PARTIAL_MODE_ON 0x12 // 部分模式开启
#define NORMAL_DISPLAY_MODE_ON 0x13 // 正常显示模式开启
#define DISPLAY_INVERSION_OFF 0x20 // 显示反转关闭
#define DISPLAY_INVERSION_ON 0x21 // 显示反转开启
#define DISPLAY_OFF 0x28 // 显示关闭
#define DISPLAY_ON 0x29 // 显示开启
#define COLUMN_ADDRESS_SET 0x2A // 列地址设置
#define ROW_ADDRESS_SET 0x2B // 行地址设置
#define MEMORY_WRITE 0x2C // 内存写入
#define PARTIAL_AREA 0x30 // 部分区域
#define VERTICAL_SCROLLING_DEFINITION 0x33 // 垂直滚动定义
#define TEARING_EFFECT_LINE_OFF 0x34 // 撕裂效果线关闭
#define TEARING_EFFECT_LINE_ON 0x35 // 撕裂效果线开启
#define MEMORY_ACCESS_CONTROL 0x36 // 内存访问控制
#define VERTICAL_SCROLLING_START_ADDRESS 0x37 // 垂直滚动起始地址
#define IDLE_MODE_OFF 0x38 // 空闲模式关闭
#define IDLE_MODE_ON 0x39 // 空闲模式开启
#define PIXEL_FORMAT_SET 0x3A // 像素格式设置
#define WRITE_MEMORY_CONTINUE 0x3C // 继续写入内存
#define SET_TEAR_SCANLINE 0x44 // 设置撕裂扫描线
#define GET_SCANLINE 0x45 // 获取扫描线
#define WRITE_DISPLAY_BRIGHTNESS 0x51 // 写入显示亮度
#define WRITE_CTRL_DISPLAY 0x53 // 写入控制显示
#define READ_ID1 0xDA // 读取 ID1
#define READ_ID2 0xDB // 读取 ID2
#define READ_ID3 0xDC // 读取 ID3

// 二级命令
#define RGB_INTERFACE_SIGNAL_CONTROL 0xB0 // RGB 接口信号控制
#define BLANKING_PORCH_CONTROL 0xB5 // 空白区域控制
#define DISPLAY_FUNCTION_CONTROL 0xB6 // 显示功能控制
#define TEARING_EFFECT_CONTROL 0xBA // 撕裂效果控制
#define INTERFACE_CONTROL 0xF6 // 接口控制

// 三级命令
#define FRAME_RATE 0xE8 // 帧率
#define SPI2DATA_CONTROL 0xE9 // SPI2DATA 控制
#define POWER_CONTROL_1 0xC1 // 电源控制 1
#define POWER_CONTROL_2 0xC3 // 电源控制 2
#define POWER_CONTROL_3 0xC4 // 电源控制 3
#define POWER_CONTROL_4 0xC9 // 电源控制 4
#define POWER_CONTROL_7 0xA7 // 电源控制 7
#define INTER_REGISTER_ENABLE1 0xFE // 内部寄存器启用 1
#define INTER_REGISTER_ENABLE2 0xEF // 内部寄存器启用 2
#define SET_GAMMA1 0xF0 // 设置伽马 1
#define SET_GAMMA2 0xF1 // 设置伽马 2
#define SET_GAMMA3 0xF2 // 设置伽马 3
#define SET_GAMMA4 0xF3 // 设置伽马 4

void LCD_Init(void);
void LCD_Clear(uint16_t Color);

void LCD_WR_DATA(uint8_t data);
void LCD_WR_DATA_16Bit(uint16_t dat);
void LCD_WR_REG(uint8_t data);

void LCD_Fill(uint16_t xsta, uint16_t ysta, uint16_t xend, uint16_t yend, uint16_t color);//填充区域
void LCD_Fill_DMA(uint16_t xsta, uint16_t ysta, uint16_t xend, uint16_t yend, uint16_t color);//DMA填充区域(同步, 等发送完)
void LCD_DMA_Write_Buffer(uint16_t color, uint32_t total_size);//DMA写入缓冲区
void LCD_Address_Set(uint16_t x, uint16_t y, uint16_t xEnd, uint16_t yEnd);//设置坐标
void LCD_drawImageWithSize_DMA(uint16_t count, uint8_t pre_count);//DMA绘制图片
void LCD_DMA_Interrupt_Init(void);//DMA中断初始化
void LCD_Address_Set_DMA(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);//DMA设置坐标
void FastFill_16bit_Repeat(uint8_t *buf, uint32_t len, uint8_t color_l, uint8_t color_h);//填充重复块
void LCD_DMA_Write_Buffer_Add_coordinates(uint16_t datah, uint16_t datal, uint32_t total_size);//DMA写入缓冲区并添加坐标信息
void LCD_Address_Set_DMA_Calculate_remaining(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);//DMA写入缓冲区并添加坐标信息
void LCD_Enter_Sleep(void);   // LCD进入睡眠低功耗

// 背光PWM控制 (PB9 → TIM4_CH4)
void LCD_Backlight_PWM_Init(void);              // 初始化TIM4_CH4 PWM输出
void LCD_Backlight_PWM_SetDuty(uint8_t duty);   // 设置占空比 0-100 (低电平占比%)
void LCD_Backlight_PWM_Stop(void);              // 停止PWM,恢复GPIO推挽输出

extern u8 shared_buffer[FILL_BUFFER_SIZE];

#endif
