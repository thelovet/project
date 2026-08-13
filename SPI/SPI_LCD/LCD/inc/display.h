#ifndef __DISPLAY_H
#define __DISPLAY_H

#include "lcd.h"
#include <stdint.h>

#define SEG_HEIGHT  24
#define START_Y     (240 - SEG_HEIGHT - 20)  

void LCD_ShowNum(uint16_t num, uint16_t y);
void LCD_ShowLowBattery(void);
void LCD_ShowLowBatteryOverlay(void);
void LCD_ClearLowBatteryOverlay(void);
void CheckAndShowLowBattery(void);
void System_PrepareForStandby(void);
void Main_ClearLowBattSaveFlag(void);

#endif
