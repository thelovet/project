/**
 * @file    key.c
 * @brief   (已废弃) 旧版物理按键驱动 —— 硬件上早已没有按键, 本模块是死代码, 2026-09 清空以省 Flash。
 *
 * 原实现(需要时从 git 历史取回: git log -- SPI/SPI_LCD/User/src/key.c):
 *   KEY_Init()  : PA0 下拉输入 + EXTI0 双边沿中断
 *   EXTI0 中断  : 抖动过滤(KEY_MIN_PRESS_MS) / 50ms 冷却 / 松手判定短按
 *   KEY_Poll()  : 主循环轮询, 按住 >= 500ms 判定长按(受 key.h 的 KEY_LONG_PRESS_ENABLE 控制)
 *   事件取出    : Get_Key_Status() / Clear_Key_Status()
 *
 * 为什么删:
 *   1) 全工程对 KEY_Init/KEY_Poll/Get_Key_Status/Clear_Key_Status/KEY_Reset 的调用点都是 0 处,
 *      PA0 的 EXTI 从未使能, 长短按永远不会发生 —— 只白占 Flash(约 134 字节 .text)。
 *   2) PA0 现在是 LCD 的 DC 脚(config.h 的 LCD_DC_PIN), 万一有人把 KEY_Init() 打开,
 *      会把 PA0 从 LCD 手里抢走(下拉输入 + 中断), 属于必须避免的隐患。
 *   3) 现在的输入来源是触摸: main.c / decompress.c 的触摸处理直接写 key_response(定义在 decompress.c)。
 *
 * 保留本文件的原因: 构建脚本的源文件列表(obj 目录里的 subdir.mk)还列着它, 删文件会报
 *   "No rule to make target"; 内容清空即不产生任何代码。
 */
