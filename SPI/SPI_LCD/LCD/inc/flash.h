#ifndef __SPI_FLASH_H
#define __SPI_FLASH_H
#include "debug.h"
#include "spi.h"
#include "config.h"


#define W25X_ReadStatusReg    0x05
#define W25X_WriteStatusReg   0x01
#define W25X_FastReadData     0x0B
#define W25X_FastReadDual     0x3B
#define W25X_PageProgram      0x02
#define W25X_BlockErase       0xD8
#define W25X_PowerDown        0xB9
#define W25X_ReleasePowerDown 0xAB
#define W25X_DeviceID         0xAB
#define W25X_ManufactDeviceID 0x90
#define W25X_JedecDeviceID    0x9F


/*1. 状态寄存器-1 (Status Register-1)
   - 忙标志 (BUSY)：指示设备是否正在执行编程、擦除或写状态寄存器指令。
   - 写使能锁存器 (WEL)：指示设备是否处于写使能状态。
   - 块保护位 (BP3, BP2, BP1, BP0)：用于控制存储器阵列的写保护。
   - 顶部/底部块保护 (TB)：控制块保护位是保护存储器阵列的顶部还是底部。
2. 状态寄存器-2 (Status Register-2)
   - 四线使能 (QE)：控制设备是否启用四线SPI操作。
   - 状态寄存器保护 (SRP, SRL)：控制状态寄存器是否被硬件或软件保护。
   - 擦除/编程暂停状态 (SUS)：指示设备是否处于擦除或编程暂停状态。
   - 安全寄存器锁位 (LB3, LB2, LB1)：用于一次性编程 (OTP) 保护安全寄存器。
3. 状态寄存器-3 (Status Register-3)
   - 当前地址模式 (ADS)：指示设备当前使用的是3字节地址模式还是4字节地址模式。
   - 上电地址模式 (ADP)：确定设备上电或复位后的初始地址模式。
   - 写保护选择 (WPS)：选择使用哪种写保护方案。
   - 输出驱动器强度 (DRV1, DRV0)：控制读操作时的输出驱动器强度。
 */

#define W25X_WriteEnable 0x06 //写使能
#define W25X_ReadData 0x03 //普读_数据
#define W25X_FastRead 0x0B //快读_数据
#define W25X_ReadStatusRegister1 0x05 //读状态寄存器-1
#define W25X_ReadStatusRegister2 0x35 //读状态寄存器-2
#define W25X_ReadStatusRegister3 0x15 //读状态寄存器-3
#define W25X_WriteStatusRegister1 0x01 //写状态寄存器-1
#define W25X_WriteStatusRegister2 0x31 //写状态寄存器-2
#define W25X_WriteStatusRegister3 0x11 //写状态寄存器-3
#define W25X_WriteEnableVolatileStatusRegister 0x50 //写使能用于易失性状态寄存器
#define W25X_WriteDisable 0x04 //写禁止
#define W25X_ReadExtendedAddressRegister 0xC8 //读扩展地址寄存器
#define W25X_WriteExtendedAddressRegister 0xC5 //写扩展地址寄存器
#define W25X_Enter4ByteAddressMode 0xB7 //进入4字节地址模式
#define W25X_Exit4ByteAddressMode 0xE9 //退出4字节地址模式
#define W25X_ReadDataWith4ByteAddress 0x13 //读数据带4字节地址
#define W25X_FastReadWith4ByteAddress 0x0C //快读带4字节地址
#define W25X_WritePage 0x02 //写_数据_0～256字节
#define W25X_WritePageWith4ByteAddress 0x12 //写_数据带4字节地址
#define W25X_SectorErase 0x20 //扇区擦除
#define W25X_SectorEraseWith4ByteAddress 0x21 //扇区擦除带4字节地址
#define W25X_32KBBlockErase 0x52 //32K块擦除
#define W25X_64KBBlockErase 0xD8 //64K块擦除
#define W25X_64KBBlockEraseWith4ByteAddress 0xDC //64K块擦除带4字节地址
#define W25X_ChipErase 0xC7 //芯片擦除
#define W25X_EraseProgramSuspend 0x75 //擦除/编程暂停
#define W25X_EraseProgramResume 0x7A //擦除/编程恢复
#define W25X_PowerDown 0xB9 //掉电
#define W25X_ReleasePowerDownDeviceID 0xAB //释放掉电/设备ID
#define W25X_ReadManufacturerDeviceID 0x90 //读制造商/设备ID
#define W25X_ReadManufacturerDeviceIDDualIO 0x92 //读制造商/设备ID双IO
#define W25X_ReadManufacturerDeviceIDQuadIO 0x94 //读制造商/设备ID四IO
#define W25X_ReadUniqueIDNumber 0x4B //读唯一ID号
#define W25X_ReadJEDECID 0x9F //读JEDEC ID
#define W25X_ReadSFDPRegister 0x5A //读SFDP寄存器
#define W25X_EraseSecurityRegister 0x44 //擦除安全寄存器
#define W25X_ProgramSecurityRegister 0x42 //编程安全寄存器
#define W25X_ReadSecurityRegister 0x48 //读安全寄存器


#define FLASH_CS_SET          (FLASH_CS_PORT->BSHR = FLASH_CS_PIN)//设置CS引脚为高电平
#define FLASH_CS_CLR          (FLASH_CS_PORT->BCR = FLASH_CS_PIN)//设置CS引脚为低电平

void SPI_FLASH_Init(void);
uint8_t SPI_Flash_ReadSR(void);
void SPI_FLASH_Write_SR(uint8_t sr);
void SPI_Flash_Wait_Busy(void);
void SPI_FLASH_Write_Enable(void);
void SPI_FLASH_Write_Disable(void);
uint16_t SPI_Flash_ReadID(void);
uint32_t SPI_Flash_ReadJedecDeviceID(void);
void SPI_Flash_Erase_Sector(uint32_t Dst_Addr);
void SPI_Flash_Read(uint8_t *pBuffer, uint32_t ReadAddr, uint16_t size);
void SPI_Flash_Write_Page(uint8_t *pBuffer, uint32_t WriteAddr, uint16_t size);
void SPI_Flash_Write_NoCheck(uint8_t *pBuffer, uint32_t WriteAddr, uint16_t size);
void SPI_Flash_Write(uint8_t *pBuffer, uint32_t WriteAddr, uint16_t size);
void SPI_Flash_Erase_Chip(void);
void SPI_Flash_PowerDown(void);
void SPI_Flash_WAKEUP(void);
void SPI_Flash_Read_dma_start(uint32_t ReadAddr);
void SPI_Flash_Read_dma_end(void);
void  FLASH_Write(uint8_t TxData);//通过SPI1只写入数据（不接收）
uint8_t FLASH_Read(void);//通过SPI1只读取数据（发送dummy）
uint8_t FLASH_NoWaiting_Read(void);//无等待读取

#endif
