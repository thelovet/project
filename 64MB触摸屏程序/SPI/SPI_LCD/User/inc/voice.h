#ifndef __VOICE_H
#define __VOICE_H

#include "ch32v20x_gpio.h"
#include "ch32v20x_rcc.h"
#include "ch32v20x_usart.h"

// ====== 语音IC开关 ======
// PB3 推挽输出，低电平打开语音IC，高电平关闭
#define VOICE_SW_RCC        RCC_APB2Periph_GPIOB
#define VOICE_SW_PORT       GPIOB
#define VOICE_SW_PIN        GPIO_Pin_3
#define VOICE_SW_ON         GPIO_ResetBits(VOICE_SW_PORT, VOICE_SW_PIN)
#define VOICE_SW_OFF        GPIO_SetBits(VOICE_SW_PORT, VOICE_SW_PIN)

// ====== 语音IC UART1 ======
// PA9 = TX, PA10 = RX
#define VOICE_UART          USART1
#define VOICE_UART_RCC      RCC_APB2Periph_USART1
#define VOICE_UART_TX_PORT  GPIOA
#define VOICE_UART_TX_PIN   GPIO_Pin_9
#define VOICE_UART_RX_PORT  GPIOA
#define VOICE_UART_RX_PIN   GPIO_Pin_10

#define VOICE_UART_BAUDRATE 9600   // 波特率(根据语音IC手册调整)

// ====== 语音IC协议 (CH7800, MP3-TF-16P兼容) ======
// 简化帧: 7E CMD DH DL EF, 无需校验和
// 文件名必须为 001.mp3 ~ 999.mp3 (纯3位数字)
#define VOICE_CMD_PLAY_TRACK   0x03  // 指定歌曲播放
#define VOICE_FRAME_START      0x7E  // 起始字节
#define VOICE_FRAME_END        0xEF  // 结束字节
#define VOICE_BOOT_TRACK       1     // 开机播放歌曲号(001)

void VOICE_SW_Init(void);     // GPIO开关初始化

#endif
