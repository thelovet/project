#include "ch32v20x_it.h"

void NMI_Handler(void)
    __attribute__((interrupt("WCH-Interrupt-fast"), used, externally_visible));
void HardFault_Handler(void)
    __attribute__((interrupt("WCH-Interrupt-fast"), used, externally_visible));

/**
 * @brief  不可屏蔽中断(NMI)处理。
 * @note   标准 ISR(保留)。
 */
void NMI_Handler(void)
{
  while (1)
  {
  }
}

/**
 * @brief  硬件错误(HardFault)中断处理。
 * @note   标准 ISR, 捕获后复位/死循环。
 */
void HardFault_Handler(void)
{
  NVIC_SystemReset();
  while (1)
  {
  }
}
