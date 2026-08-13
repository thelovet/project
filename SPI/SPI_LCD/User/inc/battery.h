#ifndef __BATTERY_H
#define __BATTERY_H

#include "lcd.h"

// 电池状态
typedef enum {
    BAT_NORMAL = 0,   // 未充电
    BAT_CHARGING,     // 充电中
    BAT_FULL          // 已充满
} BatteryStatus;

// 电池档位，具体判断电压由充电和游戏接口分别定义
typedef enum {
    BAT_LEVEL_0 = 0,
    BAT_LEVEL_1,
    BAT_LEVEL_2,
    BAT_LEVEL_3,
    BAT_LEVEL_4,
    BAT_LEVEL_5   // 满格5格 (与充电屏统一)
} BatteryLevel;

void Battery_Init(void);
void Battery_StandbyWakeupConfig(void);
BatteryStatus Battery_GetStatus(void);
uint8_t Battery_IsCharging(void);
BatteryLevel Battery_ChargeVoltageToLevel(uint16_t voltage_mv);
BatteryLevel Battery_GameVoltageToLevel(uint16_t voltage_mv);
void Battery_GameLevelAllowIncrease(void);
void Battery_DrawIcon(BatteryStatus status, BatteryLevel level);
void Battery_ResetDisplay(void);  // 打断动画时调用，强制清屏重绘

#endif
