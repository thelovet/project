/*********************************************************************
 * File Name          : spi.c
 * Author             : WCH
 * Version            : V1.0.0
 * Date               : 2023/8/15
 * Description        : file for spi.
 *********************************************************************************
* Copyright (c) 2023 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for 
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/
#include "spi.h"

static const uint8_t DummyByte = 0xFF;  /* DMA_MemoryInc_Disable 时重复发送 */


/*********************************************************************
 * @fn      SPI1_Init
 * 
 * @brief   Initialize SPI1 
 * 
 * @return  none
 */
void SPI1_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    SPI_InitTypeDef  SPI_InitStructure  = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_SPI1, ENABLE);//使能SPI1时钟

    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_5;//FLASH_CLK
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF_PP;//输出推挽模式
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    GPIOA->BSHR = GPIO_Pin_5;  // 设置为高电平

    GPIO_InitStructure.GPIO_Pin  = GPIO_Pin_6;//FLASH_MISO (输入)
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN_FLOATING;//输入浮空模式
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_7;//FLASH_MOSI (输出)
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF_PP;//输出推挽模式
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOA, &GPIO_InitStructure);
    GPIOA->BSHR = GPIO_Pin_7;  // 设置为高电平

    SPI_InitStructure.SPI_Direction = SPI_Direction_2Lines_FullDuplex;//双线全双工模式
    SPI_InitStructure.SPI_Mode      = SPI_Mode_Master;//主模式
    SPI_InitStructure.SPI_DataSize  = SPI_DataSize_8b;//8位数据
    SPI_InitStructure.SPI_CPOL      = SPI_CPOL_High;//空闲时SCK为高电平
    SPI_InitStructure.SPI_CPHA      = SPI_CPHA_2Edge;//数据在SCK第二个边沿时采样
    SPI_InitStructure.SPI_NSS       = SPI_NSS_Soft;//软件控制NSS引脚
#if SPI1_SPEED_MODE == SPI_VELOCITY_2
    SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_2;
#elif SPI1_SPEED_MODE == SPI_VELOCITY_4
    SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_4;
#elif SPI1_SPEED_MODE == SPI_VELOCITY_8
    SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_8;
#elif SPI1_SPEED_MODE == SPI_VELOCITY_16
    SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_16;
#endif

    SPI_InitStructure.SPI_FirstBit      = SPI_FirstBit_MSB;//最高位先发送
    SPI_InitStructure.SPI_CRCPolynomial = 7;//CRC多项式
    SPI_Init(SPI1, &SPI_InitStructure);//初始化SPI1
    
    SPI_Cmd(SPI1, ENABLE);//使能SPI1
    
    // 发送 dummy 数据确保 MOSI 初始为高电平
    // while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET);
    // SPI_I2S_SendData(SPI1, 0xFF);
    // while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET);

#if SPI_SPEED_MODE == SPI_FAST_MODE//快速模式
    SPI1->HSCR = 1;
#endif
}

/*********************************************************************
 * @fn      SPI1_Read_DMA_Init
 * 
 * @brief   Initialize DMA for SPI1, In this condition every time send 0XFF to read data
 * 
 * @return  none
 */
void SPI1_Read_DMA_Init()
{
    DMA_InitTypeDef DMA_InitStructure = {0};

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

    // Tx
    DMA_DeInit(SPI1_DMA_TX_CH);

    DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)&SPI1->DATAR;
    DMA_InitStructure.DMA_MemoryBaseAddr     = (uint32_t)&DummyByte;
    DMA_InitStructure.DMA_DIR                = DMA_DIR_PeripheralDST;//从外设到内存
    DMA_InitStructure.DMA_BufferSize         = 0;
    DMA_InitStructure.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;//外设地址不自增
    DMA_InitStructure.DMA_MemoryInc          = DMA_MemoryInc_Disable;//内存地址不自增
    DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;//外设数据大小为8位
    DMA_InitStructure.DMA_MemoryDataSize     = DMA_MemoryDataSize_Byte;//内存数据大小为8位
    DMA_InitStructure.DMA_Mode               = DMA_Mode_Normal;//普通模式
    DMA_InitStructure.DMA_Priority           = DMA_Priority_High;//高优先级
    DMA_InitStructure.DMA_M2M                = DMA_M2M_Disable;//非M2M模式
    DMA_Init(SPI1_DMA_TX_CH, &DMA_InitStructure);

    // Rx
    DMA_DeInit(SPI1_DMA_RX_CH);

    DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)&SPI1->DATAR;
    DMA_InitStructure.DMA_MemoryBaseAddr     = (uint32_t)NULL;
    DMA_InitStructure.DMA_DIR                = DMA_DIR_PeripheralSRC;//从外设到内存
    DMA_InitStructure.DMA_BufferSize         = 0;
    DMA_InitStructure.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;//外设地址不自增
    DMA_InitStructure.DMA_MemoryInc          = DMA_MemoryInc_Enable;//内存地址自增
    DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;//外设数据大小为8位
    DMA_InitStructure.DMA_MemoryDataSize     = DMA_MemoryDataSize_Byte;//内存数据大小为8位
    DMA_InitStructure.DMA_Mode               = DMA_Mode_Normal;//普通模式
    DMA_InitStructure.DMA_Priority           = DMA_Priority_High;//高优先级
    DMA_InitStructure.DMA_M2M                = DMA_M2M_Disable;//非M2M模式
    DMA_Init(SPI1_DMA_RX_CH, &DMA_InitStructure);//初始化SPI1 DMA接收通道2
}

/*********************************************************************
 * @fn      SPI1_Read_DMA
 * 
 * @brief   通过DMA从SPI1_FLASH读取数据
 * 
 * @param   data -  接收数据的内存地址
 *          len -  读取长度
 * 
 * @return  none
 */

void SPI1_Read_DMA(uint8_t *data, uint16_t len) {

    // 多级清空，防止硬件串扰残留
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) != RESET) {//等待接收缓冲区清空
        SPI_I2S_ReceiveData(SPI1);
    }
    SPI_Cmd(SPI1, DISABLE);
    SPI_Cmd(SPI1, ENABLE);

    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET);//等待SPI1空闲

    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) != RESET) {//等待接收缓冲区有数据
        SPI_I2S_ReceiveData(SPI1);
    }

    /* 1. 配置 RX DMA 通道的目的内存地址和传输大小 */
    DMA1_Channel2->MADDR = (uint32_t)data; // 指向你的接收缓冲区
    DMA1_Channel2->CNTR  = len;              // 接收 len 个字节

    /* 2. 配置 TX DMA 通道的源内存地址和传输大小 */
    DMA1_Channel3->MADDR = (uint32_t)&DummyByte;
    DMA1_Channel3->CNTR  = len;

    /* 3. 清除通道2的传输完成标志，然后启动两个 DMA 通道 */
    DMA_ClearFlag(DMA1_FLAG_TC2);
    DMA_Cmd(DMA1_Channel2, ENABLE);
    DMA_Cmd(DMA1_Channel3, ENABLE);

    /* 4. 等待 RX DMA 传输完成 */
    while (DMA_GetFlagStatus(DMA1_FLAG_TC2) == RESET);

    /* 5. 等待 SPI 总线空闲 */
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET);

    /* 6. 关闭 DMA 通道 */
    DMA_Cmd(DMA1_Channel2, DISABLE);
    DMA_Cmd(DMA1_Channel3, DISABLE);

    if (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) != RESET) {
        SPI_I2S_ReceiveData(SPI1);
    }
}


/*********************************************************************
 * @fn      SPI1_Read_DMA_NonBlocking
 * 
 * @brief   通过非阻塞DMA从SPI1_FLASH读取数据
 * 
 * @param   data -  接收数据的内存地址
 *          len -  读取长度
 * 
 * @return  none
 */
void SPI1_Read_DMA_NonBlocking(uint8_t* data, uint16_t len) {
    // 多级清空，防止硬件串扰残留
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) != RESET) {//等待接收缓冲区清空
        SPI_I2S_ReceiveData(SPI1);
    }
    SPI_Cmd(SPI1, DISABLE);//关闭SPI1flash，防止串扰
    SPI_Cmd(SPI1, ENABLE);//开启SPI1flash

    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET);//等待SPI1flash空闲

    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) != RESET) {//等待接收缓冲区有数据
        SPI_I2S_ReceiveData(SPI1);
    }

    /* 1. 配置 RX DMA 通道的目的内存地址和传输大小 */
    DMA1_Channel2->MADDR = (uint32_t)data; // 指向你的接收缓冲区
    DMA1_Channel2->CNTR = len;              // 接收 len 个字节

    /* 2. 配置 TX DMA 通道的源内存地址和传输大小 */
    DMA1_Channel3->MADDR = (uint32_t)&DummyByte;
    DMA1_Channel3->CNTR = len;


    /* 3. 清除通道2的传输完成标志，然后启动两个 DMA 通道 */
    DMA_ClearFlag(DMA1_FLAG_TC2); // 清除 RX 完成标志
    DMA_Cmd(DMA1_Channel2, ENABLE); // 启动 RX
    DMA_Cmd(DMA1_Channel3, ENABLE); // 启动 TX
}


/*********************************************************************
 * @fn      SPI1_DMA_WaitComplete
 * 
 * @brief   等待DMA传输完成
 * 
 * @return  none
 */
void SPI1_DMA_WaitComplete(void) {
    /* 4. 等待 RX DMA 传输完成 */
    while (DMA_GetFlagStatus(DMA1_FLAG_TC2) == RESET);

    /* 5. 等待 SPI 总线空闲（发送最后一个字节的移位完成） */
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET);

    /* 6. 关闭 DMA 通道 */
    DMA_Cmd(DMA1_Channel2, DISABLE);
    DMA_Cmd(DMA1_Channel3, DISABLE);

    // ===== 出口防护：吃掉落地的最后一个字节 =====
    if (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) != RESET) {
        SPI_I2S_ReceiveData(SPI1);
    }
}


/*********************************************************************
 * @fn      SPI1_Write_DMA
 * 
 * @brief   通过DMA发送数据到SPI1_FLASH
 * 
 * @param   data -  发送数据的内存地址
 *          len -  发送长度
 * 
 * @return  none
 */
void SPI1_Write_DMA(uint8_t* data, uint16_t len) {
    static DMA_InitTypeDef DMA_InitStructure = {0};
    
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) != RESET) {
        SPI_I2S_ReceiveData(SPI1);
    }
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET);
    
    DMA_Cmd(DMA1_Channel3, DISABLE);
    
    DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)&SPI1->DATAR;
    DMA_InitStructure.DMA_MemoryBaseAddr     = (uint32_t)data;
    DMA_InitStructure.DMA_DIR                = DMA_DIR_PeripheralDST;
    DMA_InitStructure.DMA_BufferSize         = len;
    DMA_InitStructure.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;
    DMA_InitStructure.DMA_MemoryInc          = DMA_MemoryInc_Enable;
    DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    DMA_InitStructure.DMA_MemoryDataSize     = DMA_MemoryDataSize_Byte;
    DMA_InitStructure.DMA_Mode               = DMA_Mode_Normal;
    DMA_InitStructure.DMA_Priority           = DMA_Priority_High;
    DMA_InitStructure.DMA_M2M                = DMA_M2M_Disable;
    DMA_Init(DMA1_Channel3, &DMA_InitStructure);
    
    DMA_ClearFlag(DMA1_FLAG_TC3);
    SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Tx, ENABLE);
    DMA_Cmd(DMA1_Channel3, ENABLE);
    
    while (DMA_GetFlagStatus(DMA1_FLAG_TC3) == RESET);
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET);
    
    DMA_Cmd(DMA1_Channel3, DISABLE);
    SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Tx, DISABLE);
    
    if (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) != RESET) {
        SPI_I2S_ReceiveData(SPI1);
    }
    
    DMA_InitStructure.DMA_MemoryBaseAddr     = (uint32_t)&DummyByte;
    DMA_InitStructure.DMA_MemoryInc          = DMA_MemoryInc_Disable;
    DMA_InitStructure.DMA_BufferSize         = 0;
    DMA_Init(DMA1_Channel3, &DMA_InitStructure);//初始化DMA通道3

        // 如果发送过程中产生了接收数据（通常都有），读走丢弃
    if (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) != RESET) {
        SPI_I2S_ReceiveData(SPI1);
    }
}

/*********************************************************************
 * @fn      SPI2_Init
 * 
 * @brief   Initialize SPI2
 * 
 * @return  none
 */
void SPI2_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure = {0};
    SPI_InitTypeDef  SPI_InitStructure  = {0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, ENABLE);

    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_13;//LCD_SCK
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF_PP;//复用推挽输出
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;//50MHz
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin  = GPIO_Pin_14;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;//输入上拉电阻模式
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin   = GPIO_Pin_15;//LCD_MOSI
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_AF_PP;//复用推挽输出
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;//50MHz
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    SPI_InitStructure.SPI_Direction         = SPI_Direction_1Line_Tx;//单线模式，发送数据
    SPI_InitStructure.SPI_Mode              = SPI_Mode_Master;//主模式
    SPI_InitStructure.SPI_DataSize          = SPI_DataSize_8b;//数据大小为8位
    SPI_InitStructure.SPI_CPOL              = SPI_CPOL_High;//时钟极性高电平
    SPI_InitStructure.SPI_CPHA              = SPI_CPHA_2Edge;//时钟相位第二个边沿
    SPI_InitStructure.SPI_NSS               = SPI_NSS_Soft;//软件控制NSS
    #if SPI2_SPEED_MODE == SPI_VELOCITY_2
        SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_2;
    #elif SPI2_SPEED_MODE == SPI_VELOCITY_4
        SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_4;
    #elif SPI2_SPEED_MODE == SPI_VELOCITY_8
        SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_8;
    #elif SPI2_SPEED_MODE == SPI_VELOCITY_16
        SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_16;
    #endif
    SPI_InitStructure.SPI_FirstBit          = SPI_FirstBit_MSB;//最高位先发送
    SPI_InitStructure.SPI_CRCPolynomial     = 7;//CRC多项式为7
    SPI_Init(SPI2, &SPI_InitStructure);

    SPI_Cmd(SPI2, ENABLE);//使能SPI2
}

/*********************************************************************
 * @fn      SPI2_DMA_Init
 * 
 * @brief   Initialize DMA for SPI2
 * 
 * @return  none
 */
void SPI2_DMA_Init()
{
    DMA_InitTypeDef DMA_InitStructure = {0};

    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

    DMA_DeInit(SPI2_DMA_TX_CH);

    DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)&SPI2->DATAR;//SPI2数据寄存器地址
    DMA_InitStructure.DMA_MemoryBaseAddr     = (uint32_t)NULL;//内存地址
    DMA_InitStructure.DMA_DIR                = DMA_DIR_PeripheralDST;//外设到内存
    DMA_InitStructure.DMA_BufferSize         = 0;
    DMA_InitStructure.DMA_PeripheralInc      = DMA_PeripheralInc_Disable;//外设不自增
    DMA_InitStructure.DMA_MemoryInc          = DMA_MemoryInc_Enable;//内存自增
    DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;//外设数据大小为8位
    DMA_InitStructure.DMA_MemoryDataSize     = DMA_MemoryDataSize_Byte;//内存数据大小为8位
    DMA_InitStructure.DMA_Mode               = DMA_Mode_Normal;//普通模式，不使用循环模式
    DMA_InitStructure.DMA_Priority           = DMA_Priority_High;//高优先级
    DMA_InitStructure.DMA_M2M                = DMA_M2M_Disable;//不使用M2M模式
    DMA_Init(SPI2_DMA_TX_CH, &DMA_InitStructure);

    SPI_I2S_DMACmd(SPI2, SPI_I2S_DMAReq_Tx, ENABLE);//使能SPI2 DMA
}


/*********************************************************************
 * @fn      SPI2_Write
 * 
 * @brief   send one byte
 * @brief   发送一个字节数据
 * 
 * @param   data -  the data to send
 * 
 * @return  none
 */
void SPI2_Write(uint8_t data) {
    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_TXE) == RESET)//等待发送缓冲区为空
        ;
    SPI_I2S_SendData(SPI2, data & 0xFF);  // 确保只发送低8位
    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET)//等待发送完成
        ;
}

/*********************************************************************
 * @fn      SPI2_Write_DMA
 * 
 * @brief   send a lot of data 
 * @brief   通过DMA_SPI2发送屏幕多字节数据
 * 
 * @param   data -  the data to be send 
 *          len -  the length of data
 * 
 * @return  none
 */
void SPI2_Write_DMA(uint8_t *data, uint16_t len)
{
   // 1. 等待 SPI2 完全空闲
    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);

    // 2. 清空接收缓冲区（无害）
    if (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_RXNE) != RESET) {
        SPI_I2S_ReceiveData(SPI2);
    }

    // // 3. ★ 确保发送缓冲区为空，如果非空，复位 SPI2 以清空
    if (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_TXE) == RESET) {
        SPI_Cmd(SPI2, DISABLE);
        SPI_Cmd(SPI2, ENABLE);
        while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_TXE) == RESET);
    }

    // 4. 配置并启动 DMA
    DMA_Cmd(SPI2_DMA_TX_CH, DISABLE);
    DMA_ClearFlag(SPI2_DMA_TC_FLAG);

    SPI2_DMA_TX_CH->MADDR = (uint32_t)data;//设置DMA内存地址为data指针
    SPI2_DMA_TX_CH->CNTR  = len;//设置DMA传输字节数为len

    DMA_Cmd(SPI2_DMA_TX_CH, ENABLE);//使能DMA传输
}


//配置DMA传输
void SPI2_WaitIdle(void)
{
    // 1. 等待 SPI2 完全空闲
    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);

    // 2. 清空接收缓冲区（无害）
    if (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_RXNE) != RESET) {
        SPI_I2S_ReceiveData(SPI2);
    }

    // 3. 确保发送缓冲区为空，如果非空，复位 SPI2 以清空
    if (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_TXE) == RESET) {
        SPI_Cmd(SPI2, DISABLE);
        SPI_Cmd(SPI2, ENABLE);
        while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_TXE) == RESET);
    }
}

//启动DMA传输
void SPI2_StartDMA(uint8_t *data, uint16_t len)
{
    // 4. 配置并启动 DMA
    DMA_Cmd(SPI2_DMA_TX_CH, DISABLE);//禁用DMA传输
    DMA_ClearFlag(SPI2_DMA_TC_FLAG);//清除DMA传输完成标志位

    SPI2_DMA_TX_CH->MADDR = (uint32_t)data;
    SPI2_DMA_TX_CH->CNTR = len;

    DMA_Cmd(SPI2_DMA_TX_CH, ENABLE);//使能DMA传输
}


//
void SPI2_SetDataSize(uint16_t DataSize)
{
    SPI_InitTypeDef SPI_InitStructure = {0};
    
    SPI_Cmd(SPI2, DISABLE);  // 先禁用 SPI
    
    SPI_InitStructure.SPI_Direction         = SPI_Direction_1Line_Tx;
    SPI_InitStructure.SPI_Mode              = SPI_Mode_Master;
    SPI_InitStructure.SPI_DataSize          = DataSize;
    SPI_InitStructure.SPI_CPOL              = SPI_CPOL_Low;
    SPI_InitStructure.SPI_CPHA              = SPI_CPHA_1Edge;
    SPI_InitStructure.SPI_NSS               = SPI_NSS_Soft;
    SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_2;
    SPI_InitStructure.SPI_FirstBit          = SPI_FirstBit_MSB;
    SPI_InitStructure.SPI_CRCPolynomial     = 7;
    SPI_Init(SPI2, &SPI_InitStructure);
    
    SPI_Cmd(SPI2, ENABLE);  // 重新启用 SPI
}


