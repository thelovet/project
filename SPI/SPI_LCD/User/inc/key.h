#ifndef __KEY_H
#define __KEY_H

#include "ch32v20x.h"

// 按键状态标志
// 按键类型定义
#define KEY_SHORT_PRESS        0x01    // 短按
#define KEY_LONG_PRESS         0x02    // 长按

#define KEY_LONG_PRESS_ENABLE  1  // 长按开关：1=长按有效，0=长按无效（松手触发短按）

// 按键驱动接口函数
void KEY_Init(void);
uint8_t Get_Key_Status(void);
void Clear_Key_Status(void);
void KEY_Reset(void);
void KEY_Poll(void);

extern uint8_t key_response;       // 按键响应标志

#endif