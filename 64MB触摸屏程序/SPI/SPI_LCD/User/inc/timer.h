#ifndef __TIMER_H
#define __TIMER_H

#include "ch32v20x.h"

void TIMER2_Init(void);
uint8_t TIMER2_TakeLowBatteryPending(void);
uint32_t GetTimerCount(void);
void FrameTrigger(void);
uint32_t GetFPS(void);
uint32_t GetLowestFPS(void);
void ResetFPS(void);
void ResetLowestFPS(void);
void UpdateMaxFrameTime(uint32_t duration);

#endif
