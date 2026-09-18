/*********************************************************************
 * File Name          : config.h
 * Author             : WCH
 * Version            : V1.0.0
 * Date               : 2023/8/15
 * Description        : file for Config.
 *********************************************************************************
* Copyright (c) 2023 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/
#ifndef __CONFIG_H__
#define __CONFIG_H__

/* LCD Screen Definition */
//#define USE_HORIZONTAL    2
#define LCD_W             (240u)
#define LCD_H             (240u)

/* Buffer Definition */
#define COLOR_BYTE        (2u)
#define FILL_BUFFER_COUNT (256u * 10u)
#define FILL_BUFFER_SIZE  (FILL_BUFFER_COUNT * COLOR_BYTE)

/* Parallel Buffer Definition - smaller buffer for better parallelism */
#define PARALLEL_BUFFER_COUNT (256u * 4u)
#define PARALLEL_BUFFER_SIZE  (PARALLEL_BUFFER_COUNT * COLOR_BYTE)

/* SPI Flash Speed Definition */
#define SPI_VELOCITY_2   1
#define SPI_VELOCITY_4   2
#define SPI_VELOCITY_8   3
#define SPI_VELOCITY_16  4

/* SPI1 (Flash) and SPI2 (LCD) Speed Definition */
#define SPI1_SPEED_MODE    SPI_VELOCITY_2  /* Flash: 144MHz / 2 = 72MHz */
#define SPI2_SPEED_MODE    SPI_VELOCITY_2  /* LCD: 144MHz / 2 = 72MHz */

/* SPI FLASH and LCD_SCREEN GPIO Definition */
#define FLASH_CS_PORT     GPIOA
#define FLASH_CS_RCC      RCC_APB2Periph_GPIOA
#define FLASH_CS_PIN      (GPIO_Pin_2)

#define LCD_LED_RCC       RCC_APB2Periph_GPIOB
#define LCD_LED_PORT      GPIOB
#define LCD_LED_PIN       (GPIO_Pin_9)

#define LCD_DC_RCC        RCC_APB2Periph_GPIOB
#define LCD_DC_PORT       GPIOB
#define LCD_DC_PIN        (GPIO_Pin_0)

#define LCD_RST_RCC       RCC_APB2Periph_GPIOB
#define LCD_RST_PORT      GPIOB
#define LCD_RST_PIN       (GPIO_Pin_1)

#define LCD_CS_RCC        RCC_APB2Periph_GPIOB
#define LCD_CS_PORT       GPIOB
#define LCD_CS_PIN        (GPIO_Pin_12)

#define LCD_PWR_RCC       RCC_APB2Periph_GPIOB
#define LCD_PWR_PORT      GPIOB
#define LCD_PWR_PIN       (GPIO_Pin_4)

/* 屏幕背光极性（定义=测试屏，高电平亮，注释=正常屏，低电平亮） */
//#define TEST_SCREEN_BACKLIGHT
/* Standby模式保持LCD供电，支持触摸唤醒（会增加功耗） */
#define KEEP_LCD_POWER_IN_STANDBY

/* ================================================================
 * 烧录模式切换 (二选一, 都注释 = 正常游戏程序)
 *   FLASH_DATA_STORE_MODE : 全量自动烧录内置字库/图片资源到外部Flash末尾
 *   VIDEO_CHUNK_STORE_MODE: 烧录嵌入固件的视频数据块到外部Flash地址0(视频区域)
 *                           每块由 gen_video_chunk.py 生成(video_chunk.c/h)
 * ================================================================ */
// 烧录模式:
//   FLASH_DATA_STORE_MODE  = 全量自动烧录字库/图片资源到外部Flash末尾
//   VIDEO_CHUNK_STORE_MODE = 烧录嵌入固件的视频数据块到外部Flash地址0(视频区域)
//#define FLASH_DATA_STORE_MODE   /* 字库/图片烧录模式: 开机把内置资源写进外部Flash末尾, 完成后常驻100%; 烧完必须注释回来 */
//#define VIDEO_CHUNK_STORE_MODE
/* 推箱子导航按钮图 + 撤销界面按钮图 + 菜单追加字模: 都放外部Flash的固定保留区,
   不进资源表/目录(内部Flash 装不下) → 用独立烧录模式单独烧一次, 不动字库:
   - 英文导航按钮图 5580B   → BTN_IMG_EN_ADDR
   - 撤销按钮图 中/英各 5580B → BTN_IMG_UNDO_CN_ADDR / BTN_IMG_UNDO_EN_ADDR
   - 菜单追加字模 21 字 1512B → 字库数据区紧前面
   打开下面的宏编译烧一次即可(不动目录; 字模块地址按 data_start 现算)。
   ★ 烧完必须注释回来(否则编出来的是烧录固件)。 */
//#define BTN_IMG_STORE_MODE
/* 触摸/噪声调试: 打开后视频模式不再刷视频帧(保持黑屏), 主菜单/上划设置菜单/触摸逻辑照常运行
   —— 用于隔离"显示屏刷新 + 背光 + SPI 活动"对触摸灵敏度/抖动的影响。
   建议与 main.c 顶部的 DEBUG_NO_STANDBY(无休眠) 一起用: 取消注释两者即得"无休眠 + 黑屏 + 菜单可用"的调试固件。
   注意: 打开后菜单关闭时不会再用视频帧盖掉菜单残留, 属正常现象, 只用于调试。 */
//#define DEBUG_BLACK_VIDEO   /* 调试用: 打开=视频不刷帧(黑屏), 菜单/触摸照常; 正式版必须注释掉 */

#endif
