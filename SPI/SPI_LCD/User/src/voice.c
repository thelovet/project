#include "voice.h"

/**
 * @brief  语音IC电源开关初始化, PB3推挽输出, 低电平导通(N-MOS)
 * @return 无
 */
void VOICE_SW_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    RCC_APB2PeriphClockCmd(VOICE_SW_RCC, ENABLE);
    GPIO_InitStructure.GPIO_Pin = VOICE_SW_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(VOICE_SW_PORT, &GPIO_InitStructure);
    VOICE_SW_OFF;
}

/**
 * @brief  语音IC UART1初始化, PA9(TX) PA10(RX), 波特率9600
 * @return 无
 */
void VOICE_UART_Init(void)
{
    /* UART1初始化, 波特率9600 */
    USART_Cmd(USART1, ENABLE);
}

/**
 * @brief  语音IC播放指定轨道, 格式: 7E 13 00 02 DH DL EF
 *         001.mp3 ~ 999.mp3
 * @param  track_num 轨道号(1~999)
 * @return 无
 */
void VOICE_PlayTrack(uint16_t track_num)
{
    uint8_t dh = (track_num >> 8) & 0xFF;
    uint8_t dl = track_num & 0xFF;
    uint8_t frame[] = { 0x7E, 0x13, 0x00, 0x02, dh, dl, 0xEF };
    for (uint8_t i = 0; i < 7; i++) {
        while (USART_GetFlagStatus(VOICE_UART, USART_FLAG_TXE) == RESET);
        USART_SendData(VOICE_UART, frame[i]);
    }
}
