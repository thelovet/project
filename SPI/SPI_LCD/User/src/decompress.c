#include "decompress.h"    // 解压缩算法头文件
#include "lcd.h"           // LCD显示接口
#include "SPI.h"     // SPI Flash读写接口
#include "flash.h"
#include "display.h"
#include "battery.h"
#include "timer.h"
#include "touch.h"
#include "debug.h"
#include "settings.h"


uint8_t key_response = 0;       // 触摸菜单输入响应标志
uint8_t blink_touched = 0;      // 闪烁期间被触摸，main.c读取后清零

u16  input_pos3 = 0;    // Flash地址最高位字节(32位地址的第24-32位)
u16 input_pos2 = 0;    // Flash地址次高位字节(24位地址的第16-23位)
u16 input_pos1 = 0;   // Flash地址次低位字节(8-15位)
u16 input_pos0 = 0;   // Flash地址最低位字节(0-7位)
u32 input_pos = 0;   // Flash地址(34位地址)
u16 start_x;//当前画面的起始x坐标
u16 start_y;//当前画面的起始y坐标
u16 end_x;//当前画面的结束x坐标
u16 end_y;//当前画面的结束y坐标
uint8_t end_flag = 0;//是否播放完
uint8_t lcd_blink_flag = 0;//lCD闪烁标志位
uint32_t seg_end = 0;//当前数据段结束地址
uint32_t last_seg_end = 0;//最后一个数据段结束地址
uint8_t pre_data[PARALLEL_BUFFER_COUNT];//预读数据数组

#define AVG_FRAME_COUNT       10    // 滑动窗10帧求平均值
#define TARGET_FPS            40    // 目标帧率，改这一个值即可
#define INIT_FRAME_TIME_US    (1000000 / TARGET_FPS)  // 自动计算每帧微秒数，60FPS=16666us

//当前视频段
uint8_t current_video_index = 0;
//最大视频段数
uint8_t max_video_count = VIDEO_SEGMENT_COUNT;

// 视频段地址范围定义（每组{起始地址4字节, 结束地址4字节}）
const uint8_t video_ranges[VIDEO_SEGMENT_COUNT][8] = {
	{0x00, 0x00, 0x00, 0x00, 0x00, 0x7E, 0xBC, 0x25},
	// {0x00, 0x00, 0x00, 0x00, 0x00, 0x0A, 0xD1, 0xE2},
	// {0x00, 0x0A, 0xD1, 0xE3, 0x00, 0x1D, 0x47, 0x93},
	// {0x00, 0x1D, 0x47, 0x94, 0x00, 0x30, 0xFB, 0x35},
	// {0x00, 0x30, 0xFB, 0x36, 0x00, 0x5C, 0x1E, 0x2B},
	//{0x00, 0x61, 0x07, 0xEC, 0x00, 0x6C, 0xF1, 0xFF},
};



// 帧时间样本数组，用于计算平均帧时间 (零初始化，首10帧后自然收敛)
static uint32_t frame_time_samples[AVG_FRAME_COUNT];
static uint8_t sample_index = 0;           // 当前样本索引（循环覆盖旧数据）
static uint32_t avg_frame_time = INIT_FRAME_TIME_US;    // 平均帧时间（微秒），用于动态帧率延迟


/**
 * @brief 更新平均帧时间
 *
 * 滑动窗10帧的平均时间，用于调整显示速度
 *
 * @param frame_duration 当前帧时间
 */
void UpdateAvgFrameTime(uint32_t frame_duration) {
	frame_time_samples[sample_index] = frame_duration;
	sample_index = (sample_index + 1) % AVG_FRAME_COUNT;

	uint32_t sum = 0;
	for (uint8_t i = 0; i < AVG_FRAME_COUNT; i++) {
		sum += frame_time_samples[i];
	}
	avg_frame_time = sum / AVG_FRAME_COUNT;

	if (avg_frame_time < INIT_FRAME_TIME_US) {
		avg_frame_time = INIT_FRAME_TIME_US;
	}
}

/**
 * @brief 进入4字节地址模式
 *
 * 配置SPI Flash进入4字节地址模式
 */
void byte_address_mode() {//4字节地址模式
	u16 i = 5;
	FLASH_CS_SET;//拉高Flash CS
	FLASH_CS_CLR;//拉低Flash CS选中
	FLASH_Write(W25X_WriteEnable); // 写使能
	while (i--) {
		FLASH_CS_SET;//拉高Flash CS
	}

	FLASH_CS_CLR;//拉低Flash CS选中
	FLASH_Write(W25X_Enter4ByteAddressMode); // 进入4字节地址模式
	i = 5;
	while (i--) {
		FLASH_CS_SET;//拉高Flash CS
	}
}



/**
 * @brief 设置显示范围
 *
 * 读取坐标后配置屏幕显示位置
 *
 * @param total_x 图像总宽度
 * @param picture_x 图像起始X坐标
 * @param picture_y 图像起始Y坐标
 * @param length 图像长度
 */
void Set_display_range() {//读取坐标后配置屏幕显示位置
	LCD_CS_CLR;//选中LCD
	LCD_Address_Set(start_x, start_y, end_x, end_y);//设置显示范围
	//LCD_DC_SET;//LCD数据/命令切换为高 (数据模式)
}

/**
 * @brief 设置显示范围（DMA模式）
 *
 * 读取坐标后配置屏幕显示位置，使用DMA模式传输
 *
 * @param total_x 图像总宽度
 * @param picture_x 图像起始X坐标
 * @param picture_y 图像起始Y坐标
 * @param length 图像长度
 */
void Set_display_range_DMA() {//读取坐标后配置屏幕显示位置
	//while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);//等待SPI2空闲
	LCD_CS_CLR;//选中LCD
	LCD_Address_Set_DMA_Calculate_remaining(start_x, start_y, end_x, end_y);//设置显示范围
	//LCD_Address_Set_DMA_Calculate_remaining(start_x, start_y, end_x, end_y);//设置显示范围
}

/**
 * @brief 解压缩并显示SPI Flash中的图像数据到LCD
 *
 * @param location4    待读取的flash位置高字节
 * @param location3    待读取的flash位置次高字节
 * @param location2    待读取的flash位置次低字节
 * @param location1    待读取的flash位置低字节
 *
 * 功能说明：
 * 1. 从SPI Flash读取自定义压缩格式的图像数据
 * 2. 实时解压图像数据（支持RLE重复块/非重复块）
 * 3. 将解压后的数据直接写入LCD显示缓冲区
 * 4. 使用24位全局地址指针(input_pos2/1/0)加速读取位置
 */
u16 flash_decompress_and_show(u8* location4, u16* location3, u16* location2, u16* location1) {
	u16 length = 0;      // 当前数据块的实际长度	
	uint32_t frame_start = GetTimerCount();  // 记录帧开始时间	
	u8 header = 0;     // 数据块头字节标识
	u8 header1 = 0;     // 数据块头字节标识
	u8 header2 = 0;     // 数据块头字节标识														 
	u8 is_repeat = 0;      // 数据类型标记：1=重复块，0=非重复块
	u8 len_high = 0;    // 长度扩展字节高位（当长度>127时使用）
	u8 len_low = 0;    // 长度扩展字节低位（当长度>127时使用）
	u8 datah = 0;   // 当前像素的高字节（默认值）
	u8 datal = 0;   // 当前像素的低字节（默认值）			
	uint8_t pre_count = 0;//预读像素数量

	last_seg_end = ((uint32_t)video_ranges[max_video_count - 1][4] << 24) |
		((uint32_t)video_ranges[max_video_count - 1][5] << 16) |
		((uint32_t)video_ranges[max_video_count - 1][6] << 8) |
		(uint32_t)video_ranges[max_video_count - 1][7];//最后一个数据段结束地址

	seg_end = ((uint32_t)video_ranges[current_video_index][4] << 24) |
		((uint32_t)video_ranges[current_video_index][5] << 16) |
		((uint32_t)video_ranges[current_video_index][6] << 8) |
		(uint32_t)video_ranges[current_video_index][7];//当前数据段结束地址

	input_pos3 = *location4;
	input_pos2 = *location3;
	input_pos1 = *location2;
	input_pos0 = *location1;
	input_pos = (uint32_t)input_pos3 << 24 | (uint32_t)input_pos2 << 16 | (uint32_t)input_pos1 << 8 | input_pos0;//34位地址


	//byte_address_mode();//4字节地址模式
	/* 读取块头信息（1字节） */
	LCD_CS_SET;//取消选中LCD
	FLASH_CS_CLR;//选中Flash
	//LCD_LED_CLR;//开启LCD背光

	// 使用DMA发送命令和地址，提高传输效率
	// static uint8_t flash_cmd[5];
	// flash_cmd[0] = W25X_FastRead;  // 快速读取
	// flash_cmd[1] = input_pos3;      // 32位地址
	// flash_cmd[2] = input_pos2;
	// flash_cmd[3] = input_pos1;
	// flash_cmd[4] = input_pos0;
	// SPI1_Write_DMA(flash_cmd, 5);   // DMA发送命令和地址（内部自动等待DMA结束）

	FLASH_Write(W25X_FastRead);// 发送快速读取命令（0x0B）
	//FLASH_Write(input_pos3);// 发送32位地址
	FLASH_Write(input_pos2);
	FLASH_Write(input_pos1);
	FLASH_Write(input_pos0);

	FLASH_Read();   // 丢弃0xFF等待
	SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Tx | SPI_I2S_DMAReq_Rx, ENABLE);  // 使能SPI1 DMA
	while (1) {
	next_block:
		//SPI1_DMA_WaitComplete();
		start_x = FLASH_Read();//读取画面起始位置x
		start_y = FLASH_Read();//读取画面起始位置y
		end_x = FLASH_Read();//读取画面结束位置x
		end_y = FLASH_Read();//读取画面结束位置y			
		input_pos += 4;//地址加4
		while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);//等待SPI2屏幕刷新完成

		if (start_x == 0xF1 && start_y == 0xF1) {// 检查是否是帧结束标志 (0xF1,0xF1,0xFF,0xFF,0xFF)
			if (end_x == 0xFF && end_y == 0xFF) {//符合帧结束标志
				uint32_t frame_end = GetTimerCount();//记录帧结束时间
				uint32_t frame_duration = frame_end - frame_start;//计算当前帧处理时间
				//UpdateMaxFrameTime(frame_duration);//更新最大帧耗时，用于最低帧率
				LCD_Backlight_PWM_SetDuty(g_brightness);//开启LCD背光(用户亮度)
				input_pos += 1;//地址加1

				*location4 = input_pos >> 24;//32位地址
				*location3 = input_pos >> 16;//24位地址
				*location2 = input_pos >> 8;//16位地址
				*location1 = input_pos;//8位地址

				// 检查是否到达当前段末尾，切换到下一段
				if (input_pos >= seg_end || key_response != 0) {//到达当前段末尾或收到输入响应
					*location4 = video_ranges[current_video_index][0];
					*location3 = video_ranges[current_video_index][1];
					*location2 = video_ranges[current_video_index][2];
					*location1 = video_ranges[current_video_index][3];
					input_pos = ((uint32_t)*location4 << 24) | ((uint32_t)*location3 << 16) |
						((uint32_t)*location2 << 8) | (uint32_t)*location1;
					//Delay_Ms(100);
					 if (!key_response) {//没有输入响应，播放一遍后闪烁再休眠
					// 	if (Battery_GetStatus() != BAT_CHARGING) {
					// 		/* 闪烁前已在按着: I2C触摸状态比PA8脉冲可靠, 跳过检测 */
					// 		uint8_t blink_check = !g_touch_pressed;
					// 		if (blink_check) g_touch_int_flag = 0;  /* 未在按, 清旧标志等新按下 */
					// 		uint8_t b = 2;
					// 		do {
					// 			LCD_Backlight_PWM_SetDuty(0);//灭
					// 		for (uint8_t i = 0; i < 35; i++) {
					// 			IWDG_ReloadCounter(); Delay_Ms(10);
					// 			if (Battery_GetStatus() == BAT_CHARGING || Battery_GetStatus() == BAT_FULL) {
					// 				LCD_Backlight_PWM_SetDuty(g_brightness);
					// 				goto blink_out;
					// 			}
					// 		}
					// 		LCD_Backlight_PWM_SetDuty(g_brightness);//亮
					// 		for (uint8_t i = 0; i < 35; i++) {
					// 			IWDG_ReloadCounter(); Delay_Ms(10);
					// 			if (blink_check && g_touch_int_flag) {
					// 				g_touch_int_flag = 0;
					// 				LCD_Backlight_PWM_SetDuty(g_brightness);
					// 				blink_touched = 1;
					// 				current_video_index = (current_video_index + 1) % max_video_count;
					// 				LoadVideoAddress();
					// 				key_response = 1;
					// 				goto blink_out;
					// 			}
					// 			if (Battery_GetStatus() == BAT_CHARGING || Battery_GetStatus() == BAT_FULL) {
					// 				LCD_Backlight_PWM_SetDuty(g_brightness);
					// 				goto blink_out;
					// 			}
					// 		}
					// 		} while (--b);
					// 	}
					 	end_flag = 1;//结束一个循环
					 }
					key_response = 0;//清除输入响应标志
				//blink_out:;
				}
				while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);//等待屏幕刷新完成
				//while ((SPI2_DMA_TX_CH->CNTR) > 0);//等待SPI2屏幕刷新完成
				FLASH_CS_SET;// 释放Flash
				//FrameTrigger();//触发帧率计算，用于显示帧率——已注释，不需要显示帧率
				// LCD_ShowNum(GetLowestFPS(), START_Y - SEG_HEIGHT - 4);//显示最低帧率
				// LCD_ShowNum(GetFPS(), START_Y);//显示当前帧率
				// 触摸坐标调试: 确认触摸IC正常后再取消注释
				// TouchState ts;
				// Touch_Scan(&ts);
				// if (ts.pressed) {
				// 	LCD_ShowNum(ts.y, START_Y - SEG_HEIGHT - 4);
				// 	LCD_ShowNum(ts.x, START_Y);
				// }

				SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Tx | SPI_I2S_DMAReq_Rx, DISABLE);//关闭SPI1 DMA

				// 充电检测：此时退出视频播放，由main.c中else分支接管充电显示
				if (Battery_GetStatus() == BAT_CHARGING || Battery_GetStatus() == BAT_FULL) {
					end_flag = 1;
				}

				if (frame_duration > 10000)//排除异常值（小于10ms不统计）
					UpdateAvgFrameTime(frame_duration);//更新平均帧时间

				if (frame_duration < avg_frame_time) {//等待剩余时间
					while ((GetTimerCount() - frame_end) < (avg_frame_time - frame_duration));
				}
				LCD_CS_SET;//取消选中LCD
				return 0;
			}
		}
		// 检查是否到达最后一个帧段末尾，闪烁后结束
		if (input_pos >= last_seg_end) {
			*location4 = 0;
			*location3 = 0;
			*location2 = 0;
			*location1 = 0;
			end_flag = 1;//意外越界，安全退出
			SPI_I2S_DMACmd(SPI1, SPI_I2S_DMAReq_Tx | SPI_I2S_DMAReq_Rx, DISABLE);
			LCD_CS_SET; FLASH_CS_SET;
			LCD_LED_SET;
			return 0;
		}
		Set_display_range_DMA();//设置显示范围
		while (1) {
			header = FLASH_Read();//读取头（SPI1正在读取时，SPI2正在发送）
			// 等待SPI2发送完成（DMA的BSY等待统一放回此处）
			// 在FLASH_Read()的SPI1通讯时，SPI2还剩最后几位数据
			input_pos0++;//低字节地址加1
			input_pos++;//地址加1
			is_repeat = (header & 0x80) == 0;// 数据类型判断：最高位=0表示重复块，1表示非重复块

			/* 重复块处理（相同像素值多次复制）*/
			if (is_repeat) {
				// 长重复块处理（长度128~32767）
				switch (header & 0x7F) {
				case 0:
					len_high = FLASH_Read();// 读取长度高字节
					len_low = FLASH_Read(); // 读取长度低字节
					datah = FLASH_Read(); //读取数据高字节
					datal = FLASH_Read(); //读取数据低字节
					while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);//等待SPI2屏幕刷新完成
					//while ((SPI2_DMA_TX_CH->CNTR) > 0);//等待SPI2屏幕刷新完成
					input_pos += 4;//地址加4
					length = ((u32)len_high << 8) + (u32)len_low;  // 实际长度=高字节加低字节值

					LCD_DMA_Write_Buffer_Add_coordinates((uint16_t)datah, (uint16_t)datal, length);//发送数据到LCD屏幕
					break;
				default:// 短重复块处理（长度1~127）
					header1 = FLASH_Read();//读取数据高字节
					header2 = FLASH_Read();//读取数据低字节
					while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);//等待SPI2屏幕刷新完成
					//while ((SPI2_DMA_TX_CH->CNTR) > 0);//等待SPI2屏幕刷新完成
					length = header & 0x7F;  // 直接获取长度
					datah = header1;
					datal = header2;
					input_pos += 2;//地址加2

					LCD_DMA_Write_Buffer_Add_coordinates((uint16_t)datah, (uint16_t)datal, length);//发送数据到LCD屏幕
					break;
				}
			}
			/* 非重复块处理（原始数据序列）*/
			else {
				// 长非重复块处理（长度128~255）
				switch (header & 0x7F) {
				case 0:
					header1 = FLASH_Read();//读取长度
					((uint16_t*)shared_buffer)[0] = FLASH_Read() | ((uint16_t)FLASH_Read() << 8);//预读取第一个像素
					if (header == 0xF2 && header1 == 0xF2 && (((uint16_t*)shared_buffer)[0] & 0xFF) == 0xFF && (((uint16_t*)shared_buffer)[0] >> 8) == 0xFF) {// 帧结束标志: header==0xF2 → 退出内层循环
						FLASH_Read();//读取剩下的0xFF
						input_pos += 4;//地址加4
						while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);//等待屏幕刷新完成	
						//while ((SPI2_DMA_TX_CH->CNTR) > 0);//等待SPI2屏幕刷新完成
						goto next_block;
					}

					len_low = header1;
					input_pos0++;//低字节地址加1
					input_pos++;//地址加1
					length = len_low;  // 实际长度
					input_pos += length * 2;//地址加length*2
					length--;//实际长度减1，因为已经读取了一个数据

					pre_count = 1;
					while (length > 0 && ((SPI2_DMA_TX_CH->CNTR) > 9 || length < 2)) {//屏幕在发送时或剩余很少时预读取数据
						((uint16_t*)shared_buffer)[pre_count] = FLASH_Read() | ((uint16_t)FLASH_Read() << 8);//预读取第二个像素
						length--;
						pre_count++;
					}
					// while (length--) {
					// 	LCD_WR_DATA(FLASH_Read());
					// 	LCD_WR_DATA(FLASH_Read());
					// }
					(void)SPI1->DATAR;  // 清空接收缓冲区残留数据
					LCD_drawImageWithSize_DMA(length, pre_count);//读取flash数据发送到LCD屏幕
					break;
				default:// 短非重复块处理（长度1~127）
					((uint16_t*)shared_buffer)[0] = FLASH_Read() | ((uint16_t)FLASH_Read() << 8);//预读取第一个像素
					if (header == 0xF2 && (((uint16_t*)shared_buffer)[0] & 0xFF) == 0xF2 && (((uint16_t*)shared_buffer)[0] >> 8) == 0xFF) {// 帧结束标志: header==0xF2 → 退出内层循环
						FLASH_Read();//读取剩下的0xFF
						FLASH_Read();//读取剩下的0xFF
						input_pos += 4;//地址加4
						while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) == SET);//等待屏幕刷新完成
						//while ((SPI2_DMA_TX_CH->CNTR) > 0);//等待SPI2屏幕刷新完成
						goto next_block;
					}

					length = header & 0x7F;  // 直接获取长度
					input_pos += length * 2;//地址加length*2
					length--;//实际长度减1，因为已经读取了一个数据

					pre_count = 1;
					while (length > 0 && ((SPI2_DMA_TX_CH->CNTR) > 9 || length < 2)) {//屏幕在发送时或剩余很少时预读取数据
						((uint16_t*)shared_buffer)[pre_count] = FLASH_Read() | ((uint16_t)FLASH_Read() << 8);//预读取第二个像素
						length--;
						pre_count++;
					}
					// while (length--) {
					// 	LCD_WR_DATA(FLASH_Read());
					// 	LCD_WR_DATA(FLASH_Read());
					// }
					(void)SPI1->DATAR;  // 清空接收缓冲区残留数据
					LCD_drawImageWithSize_DMA(length, pre_count);//读取flash数据发送到LCD屏幕
					break;
				}
			}
		}
	}
}
