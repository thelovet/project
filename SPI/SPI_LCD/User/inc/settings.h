#ifndef __SETTINGS_H__
#define __SETTINGS_H__

#include "ch32v20x.h"

/* 设置模式 */
typedef enum {
    SETTINGS_NONE        = 0,
    SETTINGS_BRIGHTNESS  = 1,
    SETTINGS_SLEEP_TIME  = 2,
    SETTINGS_ORIENTATION = 3,
} SettingsMode;

/* 返回按钮尺寸 (与settings.c中DrawBack一致) */
#define BACK_X  5
#define BACK_Y  5
#define BACK_W  30
#define BACK_H  22

/* 全局设置变量 */
extern uint8_t g_brightness;          /* 亮度 0-100 */
extern uint8_t g_sleep_time_index;    /* 息屏时间索引: 0=2S,1=5S,2=10S,3=20S,4=30S,5=60S */
extern uint8_t g_orientation_index;   /* 屏幕方向索引: 0=0?,1=90?,2=180?,3=270? */

/* 息屏时间值数组 (秒) */
extern const uint16_t g_sleep_times[6];

/* API */
void Settings_Enter(SettingsMode mode);
void Settings_Exit(void);
uint8_t Settings_Update(void);  /* 返回 0 表示按了返回键, 1 表示仍在设置中 */
void Settings_ApplyOrientation(uint8_t index);
void GameAutoSleep_KeepAlive(void);  /* main.c 提供: 重置休眠计时器 */

#endif
