#ifndef __SPI_H__
#define __SPI_H__

#include "debug.h"
#include "config.h"

#define SPI1_DMA_RX_CH   DMA1_Channel2
#define SPI1_DMA_TX_CH   DMA1_Channel3

#define SPI1_DMA_TC_FLAG DMA1_FLAG_TC3
#define SPI1_DMA_RC_FLAG DMA1_FLAG_TC2

#define SPI2_DMA_RX_CH   DMA1_Channel4
#define SPI2_DMA_TX_CH   DMA1_Channel5

#define SPI2_DMA_TC_FLAG DMA1_FLAG_TC5
#define SPI2_DMA_RC_FLAG DMA1_FLAG_TC4

void SPI1_Init(void);
void SPI1_Read_DMA_Init(void);
void SPI1_Read_DMA(uint8_t *data, uint16_t len);

void SPI2_Init(void);
void SPI2_DMA_Init(void);
void SPI2_Write(uint8_t data);
void SPI2_Write_DMA(uint8_t *data, uint16_t len);
void SPI2_WaitIdle(void);
void SPI2_StartDMA(uint8_t *data, uint16_t len);
void SPI2_SetDataSize(uint16_t DataSize);
void FLASH_Read_And_LCD_Write_DMA(uint16_t length);
void SPI1_DMA_WaitComplete(void);
uint8_t SPI1_DMA_IsComplete(void);
void SPI1_Read_DMA_NonBlocking(uint8_t *data, uint16_t len);
void SPI1_Write_DMA(uint8_t *data, uint16_t len);
uint8_t SPI2_DMA_IsComplete(void);
void SPI2_Write_DMA_NonBlocking(uint8_t *data, uint16_t len);


#endif
