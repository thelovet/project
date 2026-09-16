#ifndef __KEY_H
#define __KEY_H

/* (已废弃) 旧版物理按键驱动头文件 —— 见 key.c 顶部说明, 2026-09 随 key.c 一起清空。
 *
 * 原内容(需要时从 git 历史取回: git log -- SPI/SPI_LCD/User/inc/key.h):
 *   KEY_SHORT_PRESS / KEY_LONG_PRESS         按键事件标志
 *   KEY_LONG_PRESS_ENABLE                    长按开关(旧版: 1=长按有效; 现在已无按键, 无意义)
 *   KEY_Init / Get_Key_Status / Clear_Key_Status / KEY_Reset / KEY_Poll   旧按键驱动接口
 *   这些函数现在全工程 0 调用点, 已从 key.c 删除; PA0 现为 LCD 的 DC 脚, 不要再启用按键代码。
 */

#include "ch32v20x.h"

extern uint8_t key_response;       /* 触摸/菜单输入响应标志(真正定义在 decompress.c) */

#endif
