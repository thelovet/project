#include "flash.h"
#include "spi.h"

/**
 * @brief  通过SPI1只写入数据（不接收）
 * @param  TxData  要写入的数据
 * @return 无
 */
void FLASH_Write(u8 TxData)
{
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET)//等待发送缓冲区为空
        ;
    SPI_I2S_SendData(SPI1, TxData);// 发送数据
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET)//等待发送完成
        ;
    // 如果发送过程中产生了接收数据（通常都有），读走丢弃
    if (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) != RESET) {
        SPI_I2S_ReceiveData(SPI1);
    }
}

/**
 * @brief  通过SPI1只读取数据（发送dummy）
 * @param  无
 * @return 从SPI1接收的数据
 */
u8 FLASH_Read(void)
{
    // 清空可能残留的接收数据
    if (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) != RESET) {
        SPI_I2S_ReceiveData(SPI1);
    }
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET);//等待发送缓冲区为空
    SPI_I2S_SendData(SPI1, 0xFF);// 发送dummy数据
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) == RESET);//等待接收缓冲区不为空
    return SPI_I2S_ReceiveData(SPI1);
}

/**
 * @brief  通过SPI1只读取数据（发送dummy）
 * @param  无
 * @return 从SPI1接收的数据
 */
u8 FLASH_NoWaiting_Read(void)
{
    // 清空可能残留的接收数据
    // if (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) != RESET) {
    //     SPI_I2S_ReceiveData(SPI1);
    // }

    // while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET)//等待发送缓冲区为空
    //     ;
    SPI_I2S_SendData(SPI1, 0xFF);  // 发送dummy数据
    // while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_BSY) == SET)//等待发送完成
    //     ;
    // while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) == RESET)//等待接收缓冲区不为空
    //     ;
    return SPI_I2S_ReceiveData(SPI1);
}

/**
 * @brief  通过SPI1读取或写入数据
 * @param  TxData  要写入的数据
 * @return 从SPI1接收的数据
 */
static uint8_t FLASH_readWrite(uint8_t TxData)
{
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET)
        ;
    SPI_I2S_SendData(SPI1, TxData);

    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) == RESET)
        ;

    return SPI_I2S_ReceiveData(SPI1);
}

/**
 * @brief  初始化FLASH CS引脚
 * @return 无
 */
static void cs_pin_init()
{
    GPIO_InitTypeDef GPIO_InitStructure;
    RCC_APB2PeriphClockCmd(FLASH_CS_RCC, ENABLE);//使能FLASH CS引脚时钟
    GPIO_InitStructure.GPIO_Pin   = FLASH_CS_PIN;//FLASH CS引脚
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;//50MHz
    GPIO_InitStructure.GPIO_Mode  = GPIO_Mode_Out_PP;//输出推挽模式
    GPIO_Init(FLASH_CS_PORT, &GPIO_InitStructure);//初始化FLASH CS引脚
}

/**
 * @brief  初始化SPI FLASH接口
 * @return 无
 */
void SPI_FLASH_Init(void)
{
    SPI1_Init();
    SPI1_Read_DMA_Init();
    cs_pin_init();
    FLASH_CS_SET;
}

/**
 * @brief  读取W25Qxx状态寄存器
 *      BIT7  6   5   4   3   2   1   0
 *      SPR   RV  TB  BP2 BP1 BP0 WEL BUSY
 *     返回值为8位数据，包含W25Qxx的状态信息
 * @return byte  状态寄存器值
 */
uint8_t SPI_Flash_ReadSR(void)
{
    uint8_t byte = 0;

    FLASH_CS_CLR;
    FLASH_readWrite(W25X_ReadStatusReg);
    byte = FLASH_readWrite(0Xff);
    FLASH_CS_SET;

    return byte;
}

/**
 * @brief  写入W25Qxx状态寄存器
 * @param  sr  状态寄存器值
 * @return 无
 */
void SPI_FLASH_Write_SR(uint8_t sr)
{
    FLASH_CS_CLR;
    FLASH_readWrite(W25X_WriteStatusReg);
    FLASH_readWrite(sr);
    FLASH_CS_SET;
}

/**
 * @brief  等待FLASH空闲
 * @return 无
 */
void SPI_Flash_Wait_Busy(void)
{
    while ((SPI_Flash_ReadSR() & 0x01) == 0x01)
        ;
}

/**
 * @brief  使能FLASH写入
 * @return 无
 */
void SPI_FLASH_Write_Enable(void)
{
    FLASH_CS_CLR;
    FLASH_readWrite(W25X_WriteEnable);
    FLASH_CS_SET;
}

/**
 * @brief  禁用FLASH写入
 * @return 无
 */
void SPI_FLASH_Write_Disable(void)
{
    FLASH_CS_CLR;
    FLASH_readWrite(W25X_WriteDisable);
    FLASH_CS_SET;
}

/**
 * @brief  读取FLASH ID
 * @return Temp  FLASH_ID
 */
uint16_t SPI_Flash_ReadID(void)
{
    uint16_t Temp = 0;

    FLASH_CS_CLR;
    FLASH_readWrite(W25X_ManufactDeviceID);
    FLASH_readWrite(0x00);
    FLASH_readWrite(0x00);
    FLASH_readWrite(0x00);
    Temp |= FLASH_readWrite(0xFF) << 8;
    Temp |= FLASH_readWrite(0xFF);
    FLASH_CS_SET;

    return Temp;
}

/**
 * @brief  读取Jedec设备ID
 * @return JEDEC 设备ID
 */
uint32_t SPI_Flash_ReadJedecDeviceID(void)
{
    uint32_t Temp = 0;
    FLASH_CS_CLR;
    FLASH_readWrite(W25X_JedecDeviceID);
    Temp |= FLASH_readWrite(0xFF) << 16;
    Temp |= FLASH_readWrite(0xFF) << 8;
    Temp |= FLASH_readWrite(0xFF);
    FLASH_CS_SET;
    return Temp;
}

/**
 * @brief  擦除一个扇区(4K字节)
 * @param  Dst_Addr  扇区号(0~2047)
 * @return 无
 */
void SPI_Flash_Erase_Sector(uint32_t Dst_Addr)
{
    Dst_Addr *= 4096;
    SPI_FLASH_Write_Enable();
    SPI_Flash_Wait_Busy();
    FLASH_CS_CLR;
    FLASH_readWrite(W25X_SectorErase);
    FLASH_readWrite((uint8_t)((Dst_Addr) >> 16));
    FLASH_readWrite((uint8_t)((Dst_Addr) >> 8));
    FLASH_readWrite((uint8_t)Dst_Addr);
    FLASH_CS_SET;
    SPI_Flash_Wait_Busy();
}

/**
 * @brief  从FLASH读取数据
 * @param  pBuffer   读取数据缓冲区
 * @param  ReadAddr  起始地址(24bit)
 * @param  size      数据长度
 * @return 无
 */
void SPI_Flash_Read(uint8_t *pBuffer, uint32_t ReadAddr, uint16_t size)
{
    uint16_t i;

    FLASH_CS_CLR;
    FLASH_readWrite(W25X_ReadData);
    FLASH_readWrite((uint8_t)((ReadAddr) >> 16));
    FLASH_readWrite((uint8_t)((ReadAddr) >> 8));
    FLASH_readWrite((uint8_t)ReadAddr);

    for (i = 0; i < size; i++)
    {
        pBuffer[i] = FLASH_readWrite(0XFF);
    }

    FLASH_CS_SET;
}

/**
 * @brief  通过FLASH写入一页数据
 * @param  pBuffer    写入数据缓冲
 * @param  WriteAddr  起始地址(24bit)
 * @param  size       数据长度
 * @return 无
 */
void SPI_Flash_Write_Page(uint8_t *pBuffer, uint32_t WriteAddr, uint16_t size)
{
    uint16_t i;

    SPI_FLASH_Write_Enable();
    FLASH_CS_CLR;
    FLASH_readWrite(W25X_PageProgram);
    FLASH_readWrite((uint8_t)((WriteAddr) >> 16));
    FLASH_readWrite((uint8_t)((WriteAddr) >> 8));
    FLASH_readWrite((uint8_t)WriteAddr);

    for (i = 0; i < size; i++)
    {
        FLASH_readWrite(pBuffer[i]);
    }

    FLASH_CS_SET;
    SPI_Flash_Wait_Busy();
}

/**
 * @brief  通过FLASH写入数据(需要先擦除)
 * @note   地址范围内数据需为 0xFF
 * @param  pBuffer    写入数据缓冲
 * @param  WriteAddr  起始地址(24bit)
 * @param  size       数据长度
 * @return 无
 */
void SPI_Flash_Write_NoCheck(uint8_t *pBuffer, uint32_t WriteAddr, uint16_t size)
{
    uint16_t pageremain;

    pageremain = 256 - WriteAddr % 256;

    if (size <= pageremain)
        pageremain = size;

    while (1)
    {
        SPI_Flash_Write_Page(pBuffer, WriteAddr, pageremain);

        if (size == pageremain)
        {
            break;
        }
        else
        {
            pBuffer += pageremain;
            WriteAddr += pageremain;
            size -= pageremain;

            if (size > 256)
                pageremain = 256;
            else
                pageremain = size;
        }
    }
}

static uint8_t W25QXX_BUFFER[4096];
/**
 * @brief  通过FLASH写入数据(不需先擦除)
 * @param  pBuffer    写入数据缓冲
 * @param  WriteAddr  起始地址(24bit)
 * @param  size       数据长度
 * @return 无
 */
void SPI_Flash_Write(uint8_t *pBuffer, uint32_t WriteAddr, uint16_t size)
{
    uint32_t secpos;
    uint16_t secoff;
    uint16_t secremain;
    uint16_t i;

    secpos    = WriteAddr / 4096;
    secoff    = WriteAddr % 4096;
    secremain = 4096 - secoff;

    if (size <= secremain)
        secremain = size;

    while (1)
    {
        SPI_Flash_Read(W25QXX_BUFFER, secpos * 4096, 4096);

        for (i = 0; i < secremain; i++)
        {
            if (W25QXX_BUFFER[secoff + i] != 0XFF)
                break;
        }

        if (i < secremain)
        {
            SPI_Flash_Erase_Sector(secpos);

            for (i = 0; i < secremain; i++)
            {
                W25QXX_BUFFER[i + secoff] = pBuffer[i];
            }

            SPI_Flash_Write_NoCheck(W25QXX_BUFFER, secpos * 4096, 4096);
        }
        else
        {
            SPI_Flash_Write_NoCheck(pBuffer, WriteAddr, secremain);
        }

        if (size == secremain)
        {
            break;
        }
        else
        {
            secpos++;
            secoff = 0;

            pBuffer += secremain;
            WriteAddr += secremain;
            size -= secremain;

            if (size > 4096)
            {
                secremain = 4096;
            }
            else
            {
                secremain = size;
            }
        }
    }
}

/**
 * @brief  擦除所有FLASH页面
 * @return 无
 */
void SPI_Flash_Erase_Chip(void)
{
    SPI_FLASH_Write_Enable();
    SPI_Flash_Wait_Busy();
    FLASH_CS_CLR;
    FLASH_readWrite(W25X_ChipErase);
    FLASH_CS_SET;
    SPI_Flash_Wait_Busy();
}

/**
 * @brief  进入低功耗模式
 * @return 无
 */
void SPI_Flash_PowerDown(void)
{
    FLASH_CS_CLR;
    FLASH_readWrite(W25X_PowerDown);
    FLASH_CS_SET;
    Delay_Us(10);
}

/**
 * @brief  从低功耗模式唤醒
 * @return 无
 */
void SPI_Flash_WAKEUP(void)
{
    FLASH_CS_CLR;
    FLASH_readWrite(W25X_ReleasePowerDown);
    FLASH_CS_SET;
    Delay_Us(10);
}

/**
 * @brief  使SPI DMA并设置读取地址
 *     读取数据时，SPI DMA会自动将数据从FLASH读取到DMA缓冲区中
 * @param  ReadAddr  SPI Flash 读取地址
 * @return 无
 */
void SPI_Flash_Read_dma_start(uint32_t ReadAddr)
{
    FLASH_CS_CLR;
    FLASH_readWrite(W25X_FastReadData);
    FLASH_readWrite((uint8_t)((ReadAddr) >> 16));
    FLASH_readWrite((uint8_t)((ReadAddr) >> 8));
    FLASH_readWrite((uint8_t)ReadAddr);
    FLASH_readWrite(0xFF);
    SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Tx | SPI_I2S_DMAReq_Rx, ENABLE);
}

/**
 * @brief  禁用SPI DMA
 *     禁用SPI DMA后，SPI DMA会停止将数据从FLASH读取到DMA缓冲区中
 * @return 无
 */
void SPI_Flash_Read_dma_end(void)
{
    FLASH_CS_SET;
    SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Tx | SPI_I2S_DMAReq_Rx, DISABLE);
}
