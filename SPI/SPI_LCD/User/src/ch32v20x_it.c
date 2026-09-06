/********************************** (C) COPYRIGHT *******************************
 * File Name          : ch32v20x_it.c
 * Author             : WCH
 * Version            : V1.0.0
 * Date               : 2023/12/29
 * Description        : Main Interrupt Service Routines.
*********************************************************************************
* Copyright (c) 2021 Nanjing Qinheng Microelectronics Co., Ltd.
* Attention: This software (modified or not) and binary are used for
* microcontroller manufactured by Nanjing Qinheng Microelectronics.
*******************************************************************************/
#include "ch32v20x_it.h"

void NMI_Handler(void)
    __attribute__((interrupt("WCH-Interrupt-fast"), used, externally_visible));
void HardFault_Handler(void)
    __attribute__((interrupt("WCH-Interrupt-fast"), used, externally_visible));

/*********************************************************************
 * @fn      NMI_Handler
 *
 * @brief   This function handles NMI exception.
 *
 * @return  none
 */
/**
 * @brief  不可屏蔽中断处理。
 * @note   标准 ISR(保留)。
 */
void NMI_Handler(void)
{
  while (1)
  {
  }
}

/*********************************************************************
 * @fn      HardFault_Handler
 *
 * @brief   This function handles Hard Fault exception.
 *
 * @return  none
 */
/**
 * @brief  硬件错误中断处理。
 * @note   标准 ISR, 捕获后死循环/复位。
 */
void HardFault_Handler(void)
{
  NVIC_SystemReset();
  while (1)
  {
  }
}
