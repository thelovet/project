/**
 * @file    flash_store.c
 * @brief   Flash数据存储程序
 * @note    将固件内所有字库资源与图片数据烧录到外部SPI Flash。
 *          在main.c中定义 FLASH_DATA_STORE_MODE 后进入本模式执行烧录,
 *          屏幕上实时显示进度百分比, 烧录+校验完成后显示100%。
 *          注释掉 FLASH_DATA_STORE_MODE 即恢复正常游戏程序。
 *
 *          存储布局(从64MB Flash末尾往前):
 *          [数据区: 从data_start顺序写入各资源] [目录区: 位于Flash最末尾]
 *          data_start = FLASH_SIZE - 目录大小 - 数据总大小
 */

#include "flash_store.h"
#include "flash.h"
#include "lcd.h"
#include "display.h"       /* LCD_ShowNum(串口烧录收包计数) */
#include "timer.h"         /* GetTimerCount(串口空闲超时) */
#include "debug.h"
#include "ch32v20x_usart.h"  /* 串口烧录: USART1 */

#include "chinese_fonts.h"   /* 24x24中文点阵字库 */
#include "sok_img_win.h"     /* 32x32中文字库 + 16x32数字字库 */
#include "sokoban.h"         /* 推箱子图像数据 */
#include "sok_img_prev.h"    /* 上一关图标 */
#include "sok_img_next.h"    /* 下一关图标 */
#include "sok_img_restart.h" /* 重新开始图标 */
#include "fish_imgs.h"       /* 大鱼吃小鱼图片 */
#include "player_imgs.h"     /* 大鱼吃小鱼玩家形象 */

/* ================================================================
 * 资源表: 固件内全部需要烧录的字库与图片数据
 * ================================================================ */
typedef struct {
    const void *data;   /* 数据源指针(内部Flash const区) */
    uint32_t    size;   /* 数据长度(字节) */
} FlashStoreRes;

#ifdef FLASH_DATA_STORE_MODE
#include "sokoban.h"      /* 只为取 SOKOBAN_MAP_RES_LIST: 50 张推箱子关卡地图(见下表末尾) */

static FlashStoreRes s_resources[] = {   /* 非 const: 关卡地图的长度在运行期补(见 FlashStore_BurnAll) */
    /* ---- 24x24中文点阵字库 (chinese_fonts.c, 72字节/字) ---- */
    { font_tui,     72 }, { font_xiang,   72 }, { font_zi,     72 },
    { font_shi,     72 }, { font_pin,     72 }, { font_fan,    72 },
    { font_chong,   72 }, { font_xin2,    72 }, { font_kai2,   72 },
    { font_shi3,    72 }, { font_hui,     72 },
    { font_liang,   72 }, { font_du,      72 }, { font_she,    72 },
    { font_zhi,     72 }, { font_xi2,     72 }, { font_ping,   72 },
    { font_shi2,    72 }, { font_jian,    72 }, { font_mu,     72 },
    { font_fan2,    72 }, { font_xiang2,  72 },
    /* ---- 32x32中文字库 (sok_img_win.c, 128字节/字) ---- */
    { font_gong,    128 }, { font_xi,     128 }, { font_tong,   128 },
    { font_guan,    128 }, { font_ka,     128 }, { font_you,    128 },
    { font_xi3,     128 }, { font_jie,    128 }, { font_shu,    128 },
    /* ---- 16x32数字字库 ---- */
    { font_digits,  640 },
    /* ---- 推箱子图像数据 (sok_img_*.c) ---- */
    { sok_img_bg_palette,             103 * 2 },
    { sok_img_bg,                     SOK_TILE_PIXELS },
    { sok_img_wall_palette,            54 * 2 },
    { sok_img_wall,                    SOK_TILE_PIXELS },
    { sok_img_box_palette,            175 * 2 },
    { sok_img_box,                     SOK_TILE_PIXELS },
    { sok_img_box_on_target_palette,   25 * 2 },
    { sok_img_box_on_target,           SOK_TILE_PIXELS },
    { sok_img_target_palette,          36 * 2 },
    { sok_img_target,                  SOK_TARGET_PIXELS },
    { sok_img_player,                  SOK_PLAYER_PIXELS * 2 },
    /* ---- 导航图标 (30x31 RGB565, 中文那套: 上一关/下一关/重新开始; 英文那套见 flash_store.h
       的 BTN_IMG_EN_ADDR 保留区, 用独立模式烧) ---- */
    { sok_img_prev,     PREV_IMG_W * PREV_IMG_H * 2 },
    { sok_img_next,     NEXT_IMG_W * NEXT_IMG_H * 2 },
    { sok_img_restart,  RESTART_IMG_W * RESTART_IMG_H * 2 },
    /* ---- 大鱼吃小鱼 10条鱼图片 (RGB565 小端序, 白色透明) ---- */
    { fish_img_0, FISH_0_W * FISH_0_H * 2 },
    { fish_img_1, FISH_1_W * FISH_1_H * 2 },
    { fish_img_2, FISH_2_W * FISH_2_H * 2 },
    { fish_img_3, FISH_3_W * FISH_3_H * 2 },
    { fish_img_4, FISH_4_W * FISH_4_H * 2 },
    { fish_img_5, FISH_5_W * FISH_5_H * 2 },
    { fish_img_6, FISH_6_W * FISH_6_H * 2 },
    { fish_img_7, FISH_7_W * FISH_7_H * 2 },
    { fish_img_8, FISH_8_W * FISH_8_H * 2 },
    { fish_img_9, FISH_9_W * FISH_9_H * 2 },
    /* ---- 大鱼吃小鱼 9张玩家形象 (RGB565 小端序, 白色透明) ---- */
    { player_img_0, PLAYER_0_W * PLAYER_0_H * 2 },
    { player_img_1, PLAYER_1_W * PLAYER_1_H * 2 },
    { player_img_2, PLAYER_2_W * PLAYER_2_H * 2 },
    { player_img_3, PLAYER_3_W * PLAYER_3_H * 2 },
    { player_img_4, PLAYER_4_W * PLAYER_4_H * 2 },
    { player_img_5, PLAYER_5_W * PLAYER_5_H * 2 },
    { player_img_6, PLAYER_6_W * PLAYER_6_H * 2 },
    { player_img_7, PLAYER_7_W * PLAYER_7_H * 2 },
    { player_img_8, PLAYER_8_W * PLAYER_8_H * 2 },

    /* ---- 50 张推箱子关卡地图 (sokoban_maps.c, Bcd 压缩每字节2格) ----
       ★ 顺序必须与 flash_store.h 里 FLASH_STORE_ID_SOK_MAP_0 起的 50 个 ID 一致
       ★ 游戏模式不引用这些数组(改用外部 Flash 读取) → 会被 --gc-sections 回收, 不占内部 Flash;
         只有本烧录固件带着它们(约 3KB), 这就是"关卡搬到外部 Flash 省空间"的实现 */
#define SOK_RES_ENTRY(n)  { sok_map##n, 0 },   /* 长度在运行期补: 见 FlashStore_BurnAll 开头的 0.2 步 */
    SOKOBAN_MAP_RES_LIST(SOK_RES_ENTRY)
#undef SOK_RES_ENTRY

    /* ---- "语言设置"新增字形 (chinese_fonts.c, 语/言/中/文 + English 半宽字母) ----
       ★ 顺序必须与 flash_store.h 里 FLASH_STORE_ID_24_YU2 起的 11 个 ID 一致 */
    { font_yu2,     72 }, { font_yan,   72 },
    { font_zhong,   72 }, { font_wen,   72 },
    { font_lat_E,   48 }, { font_lat_n, 48 }, { font_lat_g, 48 },
    { font_lat_l,   48 }, { font_lat_i, 48 }, { font_lat_s, 48 },
    { font_lat_h,   48 },

    /* English UI glyph sets (one resource per set) */
    { font_latin24, LATIN24_COUNT * LATIN24_CHAR },
    { font_latin32, LATIN32_COUNT * LATIN32_CHAR },
};
#endif /* FLASH_DATA_STORE_MODE */

#define FLASH_STORE_RES_COUNT  (FLASH_STORE_ID_COUNT)  /* 资源总数 = 枚举ID数量(编译期一致) */

#ifdef FLASH_DATA_STORE_MODE
/* 资源表数量必须与flash_store.h中定义的资源ID数量一致(编译期检查) */
typedef char flash_store_count_check[
    ((FLASH_STORE_ID_COUNT == FLASH_STORE_RES_COUNT) ? 1 : -1)];
#endif /* FLASH_DATA_STORE_MODE */

/* 目录缓冲(位于Flash最末尾) */
#define FLASH_STORE_DIR_SIZE \
    (FLASH_STORE_DIR_HEADER_SIZE + FLASH_STORE_RES_COUNT * FLASH_STORE_ENTRY_SIZE)
static uint8_t s_dir_buf[FLASH_STORE_DIR_SIZE];

/* 运行期共享scratch缓冲区(最大资源: 玩家52x41 图片4264字节) */
#define FLASH_STORE_SCRATCH_SIZE  4352
static uint8_t s_scratch[FLASH_STORE_SCRATCH_SIZE];

/* ================================================================
 * CRC16 (MODBUS: 多项式0x8005反向0xA001, 初值0xFFFF)
 * 仅烧录模式使用(写入/校验), 游戏模式不编译以消除警告
 * ================================================================ */
#if defined(FLASH_DATA_STORE_MODE) || defined(VIDEO_CHUNK_STORE_MODE) \
    || defined(BTN_IMG_STORE_MODE)
/**
 * @brief  CRC16 迭代更新(每字节)。
 * @param  crc  当前 CRC
 * @param  byte 待更新字节
 * @return 更新后 CRC
 */
static uint16_t flash_store_crc16_update(uint16_t crc, uint8_t byte)
{
    crc ^= byte;
    for (uint8_t i = 0; i < 8; i++) {
        if (crc & 1) crc = (crc >> 1) ^ 0xA001;
        else         crc >>= 1;
    }
    return crc;
}
/**
 * @brief  计算一段数据的 CRC16。
 * @param  data 数据指针
 * @param  len  长度
 * @return CRC16 值
 */
static uint16_t flash_store_crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFF;
    while (len--) crc = flash_store_crc16_update(crc, *data++);
    return crc;
}
#endif /* FLASH_DATA_STORE_MODE || VIDEO_CHUNK_STORE_MODE || BTN_IMG_STORE_MODE */

/* ================================================================
 * 4字节地址SPI Flash访问 (兼容 >16MB 地址区域, 不改变芯片地址模式)
 * 擦除/写入仅烧录模式使用; 读取游戏模式也用, 不参与条件编译
 * ================================================================ */
#if defined(FLASH_DATA_STORE_MODE) || defined(VIDEO_CHUNK_STORE_MODE) \
    || defined(BTN_IMG_STORE_MODE)
/**
 * @brief  用 4B 指令擦除外部 Flash 扇区。
 * @param  addr 扇区地址
 */
static void flash_store_erase_sector_4b(uint32_t addr)
{
    SPI_FLASH_Write_Enable();
    SPI_Flash_Wait_Busy();
    FLASH_CS_CLR;
    FLASH_Write(W25X_SectorEraseWith4ByteAddress);
    FLASH_Write((uint8_t)(addr >> 24));
    FLASH_Write((uint8_t)(addr >> 16));
    FLASH_Write((uint8_t)(addr >> 8));
    FLASH_Write((uint8_t)addr);
    FLASH_CS_SET;
    SPI_Flash_Wait_Busy();
}

/**
 * @brief  用 4B 指令编程一页数据。
 * @param  src  源数据
 * @param  addr 目标地址
 * @param  len  长度
 */
static void flash_store_page_program_4b(const uint8_t *src, uint32_t addr, uint16_t len)
{
    SPI_FLASH_Write_Enable();
    FLASH_CS_CLR;
    FLASH_Write(W25X_WritePageWith4ByteAddress);
    FLASH_Write((uint8_t)(addr >> 24));
    FLASH_Write((uint8_t)(addr >> 16));
    FLASH_Write((uint8_t)(addr >> 8));
    FLASH_Write((uint8_t)addr);
    for (uint16_t i = 0; i < len; i++) FLASH_Write(src[i]);
    FLASH_CS_SET;
    SPI_Flash_Wait_Busy();
}

/**
 * @brief  用 4B 指令写入数据(自动擦页+编程)。
 * @param  src  源数据
 * @param  addr 目标地址
 * @param  len  长度
 */
static void flash_store_write_4b(const uint8_t *src, uint32_t addr, uint32_t len)
{
    while (len) {
        uint16_t chunk = (uint16_t)(256 - (addr & 0xFF)); /* 本页剩余字节数 */
        if (chunk > len) chunk = (uint16_t)len;
        flash_store_page_program_4b(src, addr, chunk);
        src  += chunk;
        addr += chunk;
        len  -= chunk;
    }
}
#endif /* FLASH_DATA_STORE_MODE || VIDEO_CHUNK_STORE_MODE || BTN_IMG_STORE_MODE */

/**
 * @brief  用 4B 指令读取外部 Flash 数据。
 * @param  buf  输出缓冲
 * @param  addr 读取地址
 * @param  len  长度
 */
static void flash_store_read_4b(uint8_t *buf, uint32_t addr, uint16_t len)
{
    FLASH_CS_CLR;
    FLASH_Write(W25X_ReadDataWith4ByteAddress);
    FLASH_Write((uint8_t)(addr >> 24));
    FLASH_Write((uint8_t)(addr >> 16));
    FLASH_Write((uint8_t)(addr >> 8));
    FLASH_Write((uint8_t)addr);
    for (uint16_t i = 0; i < len; i++) buf[i] = FLASH_Read();
    FLASH_CS_SET;
}

/* ================================================================
 * 芯片容量检测 + 统一地址访问封装
 * 多数设备为8MB(W25Q64, 仅支持3字节地址); >16MB芯片才用4字节地址
 * ================================================================ */
static uint32_t s_flash_size = FLASH_STORE_FLASH_SIZE; /* 实际容量(检测后更新) */
static uint8_t  s_use_4b     = 0;                       /* 1=使用4字节地址指令 */

/**
 * @brief  检测外部 Flash 芯片型号(读 ID)。
 */
static void flash_store_detect_chip(void)
{
    uint32_t jedec = SPI_Flash_ReadJedecDeviceID();
    switch (jedec & 0xFF) {
    case 0x17: s_flash_size = 8UL  * 1024 * 1024; break;  /* W25Q64  (64Mbit) */
    case 0x18: s_flash_size = 16UL * 1024 * 1024; break;  /* W25Q128 (128Mbit) */
    case 0x19: s_flash_size = 32UL * 1024 * 1024; break;  /* W25Q256 (256Mbit) */
    case 0x20: s_flash_size = 64UL * 1024 * 1024; break;  /* W25Q512 (512Mbit) */
    default:   s_flash_size = FLASH_STORE_FLASH_SIZE; break; /* 未知芯片: 用宏默认值 */
    }
    s_use_4b = (s_flash_size > 16UL * 1024 * 1024);
}

/* ================================================================
 * 统一扇区擦除/跨页写 (仅烧录模式使用)
 * ================================================================ */
#if defined(FLASH_DATA_STORE_MODE) || defined(VIDEO_CHUNK_STORE_MODE) \
    || defined(BTN_IMG_STORE_MODE)
/**
 * @brief  擦除外部 Flash 扇区(普通指令)。
 * @param  addr 扇区地址
 */
static void flash_store_erase_sector(uint32_t addr)
{
    if (s_use_4b) {
        flash_store_erase_sector_4b(addr);
    } else {
        SPI_Flash_Erase_Sector(addr / 4096);   /* 3字节地址, 参数为扇区序号 */
    }
}

/**
 * @brief  写入数据到外部 Flash(普通指令)。
 * @param  src  源数据
 * @param  addr 目标地址
 * @param  len  长度
 */
static void flash_store_write(const uint8_t *src, uint32_t addr, uint32_t len)
{
    if (s_use_4b) {
        flash_store_write_4b(src, addr, len);
    } else {
        SPI_Flash_Write_NoCheck((uint8_t *)src, addr, (uint16_t)len);
    }
}
#endif /* FLASH_DATA_STORE_MODE || VIDEO_CHUNK_STORE_MODE || BTN_IMG_STORE_MODE */

/**
 * @brief  读取外部 Flash 数据(普通指令)。
 * @param  buf  输出缓冲
 * @param  addr 读取地址
 * @param  len  长度
 */
static void flash_store_read(uint8_t *buf, uint32_t addr, uint16_t len)
{
    if (s_use_4b) {
        flash_store_read_4b(buf, addr, len);
    } else {
        SPI_Flash_Read_dma_start(addr); /* 发FastRead命令+地址+dummy, 使能SPI DMA */
        SPI1_Read_DMA(buf, len);        /* DMA读len字节(TX发0xFF提供时钟, RX收数据) */
        SPI_Flash_Read_dma_end();       /* CS拉高, 禁用SPI DMA */
    }
}

/* ================================================================
 * 运行期资源读取 (游戏模式: 从外部Flash按ID读取已烧录的字库/图片)
 * ================================================================ */
static uint8_t  s_res_valid = 0;     /* 目录缓存是否有效 */
static uint32_t s_data_start = 0;    /* 数据区起始地址(来自目录头) */

/**
 * @brief  读取资源目录项中的字段。
 * @param  e   目录项指针
 * @param  off 字段偏移
 * @return 字段值
 */
static uint32_t flash_store_dir_field(const uint8_t *e, uint8_t off)
{
    return ((uint32_t)e[off] << 24) | ((uint32_t)e[off + 1] << 16) |
           ((uint32_t)e[off + 2] << 8) | (uint32_t)e[off + 3];
}
/**
 * @brief  初始化外部 Flash 资源表(探测芯片、建立索引)。
 * @return 1=成功, 0=失败
 */
uint8_t FlashStore_ResInit(void)
{
    uint8_t try;
    for (try = 0; try < 3; try++) {
        flash_store_detect_chip();   /* 识别芯片容量与地址模式 */

        uint32_t dir_addr = s_flash_size - FLASH_STORE_DIR_SIZE;
        flash_store_read(s_dir_buf, dir_addr, FLASH_STORE_DIR_SIZE);

        if (s_dir_buf[0] == FLASH_STORE_DIR_MAGIC0 &&
            s_dir_buf[1] == FLASH_STORE_DIR_MAGIC1 &&
            s_dir_buf[2] == FLASH_STORE_DIR_VERSION &&
            s_dir_buf[3] == (uint8_t)FLASH_STORE_RES_COUNT) {
            s_data_start = flash_store_dir_field(s_dir_buf, 4);
            s_res_valid  = 1;
            return 1;
        }
        /* 开机早期Flash刚从PowerDown唤醒可能未就绪, 延时后重试 */
        Delay_Ms(20);
    }
    s_res_valid = 0;
    return 0;
}
/**
 * @brief  流式读取资源数据(经 SPI DMA)。
 * @param  id     资源 ID
 * @param  offset 资源内偏移
 * @param  buf    输出缓冲
 * @param  len    长度
 * @return 实际读取字节数
 */
uint16_t FlashStore_ResRead(FlashStoreId id, uint32_t offset, uint8_t *buf, uint16_t len)
{
    if (!s_res_valid || (uint32_t)id >= FLASH_STORE_RES_COUNT) return 0;

    const uint8_t *e = &s_dir_buf[FLASH_STORE_DIR_HEADER_SIZE + (uint32_t)id * FLASH_STORE_ENTRY_SIZE];
    uint32_t off    = flash_store_dir_field(e, 4);   /* 相对数据区起始的偏移 */
    uint32_t length = flash_store_dir_field(e, 8);   /* 资源长度 */
    if (offset >= length) return 0;

    uint16_t n = len;
    if ((uint32_t)n > length - offset) n = (uint16_t)(length - offset);
    flash_store_read(buf, s_data_start + off + offset, n);
    return n;
}

const uint8_t *FlashStore_ResLoad(FlashStoreId id, uint32_t offset, uint16_t len)
{
    if (len > FLASH_STORE_SCRATCH_SIZE) return 0;
    if (FlashStore_ResRead(id, offset, s_scratch, len) != len) return 0;
    return s_scratch;
}

/**
 * @brief  按绝对地址读一段外部 Flash 数据(不走资源目录)
 * @note   供固定保留区使用(如 BTN_IMG_EN_ADDR 的英文按钮图): 这些数据不在资源表/目录里,
 *         字库不重烧也能重新烧它们(独立烧录模式 BTN_IMG_STORE_MODE 写入)
 */
uint16_t FlashStore_RawRead(uint32_t addr, uint8_t *buf, uint16_t len)
{
    flash_store_read(buf, addr, len);
    return len;
}

/**
 * @brief  菜单"游戏名"追加字模的存放地址 (字库数据区紧前面的那一小块)
 * @note   地址 = (data_start 向下取整到 4KB) - MENU_GLYPH_SIZE。
 *         data_start 来自目录头(FlashStore 已缓存到 s_data_start), 所以游戏和烧录固件
 *         两边算出来的地址完全一致; 放在扇区边界之前 = 落在"全量烧字库"的擦除范围之外。
 */
static uint32_t flash_store_glyph_addr_from(uint32_t data_start)
{
    return (uint32_t)((data_start & ~(uint32_t)4095) - MENU_GLYPH_SIZE);
}

/**
 * @brief  读取菜单游戏名追加字模(10 个 24x24 汉字)
 * @retval 1=读完(没烧过时内容为 0xFF, 字不显示, 其它功能不受影响)
 */
uint8_t FlashStore_MenuGlyphRead(uint8_t *buf)
{
    flash_store_read(buf, flash_store_glyph_addr_from(s_data_start), (uint16_t)MENU_GLYPH_SIZE);
    return 1;
}

uint8_t *FlashStore_Scratch(void)
{
    return s_scratch;
}

/* ================================================================
 * 进度条UI (240x240屏幕, 仅烧录模式使用)
 * ================================================================ */
#if defined(FLASH_DATA_STORE_MODE) || defined(VIDEO_CHUNK_STORE_MODE) \
    || defined(BTN_IMG_STORE_MODE)
#define UI_BAR_X0      17   /* 进度条填充区左边界 */
#define UI_BAR_X1      225  /* 进度条填充区右边界(不含) */
#define UI_BAR_Y0      92   /* 进度条填充区上边界 */
#define UI_BAR_Y1      150  /* 进度条填充区下边界(不含) */
#define UI_BAR_WIDTH   (UI_BAR_X1 - UI_BAR_X0)

static uint8_t  s_ui_last_pct  = 0xFF; /* 上次绘制百分比(仅变化时重绘) */
static uint16_t s_ui_last_fill = 0;    /* 上次已填充宽度 */

/**
 * @brief  绘制烧录百分比文字。
 * @param  x     位置 X
 * @param  y     位置 Y
 * @param  color 文字颜色
 */
static void flash_store_draw_percent(uint16_t x, uint16_t y, uint16_t color)
{
    LCD_Fill(x,     y,      x + 4, y + 4, color); /* 右上圆点 */
    LCD_Fill(x + 2, y + 14, x + 6, y + 18, color); /* 左下圆点 */
    for (uint8_t i = 0; i < 7; i++) {              /* 斜线 */
        LCD_Fill(x + 1 + i * 2, y + 3 + i * 2, x + 3 + i * 2, y + 5 + i * 2, color);
    }
}

/**
 * @brief  初始化烧录进度 UI(背景/标题)。
 */
static void flash_store_ui_init(void)
{
    s_ui_last_pct  = 0xFF;
    s_ui_last_fill = 0;
    LCD_Fill(0, 0, LCD_W, LCD_H, LCD_BLACK);
    /* 进度条边框(白色) */
    LCD_Fill(UI_BAR_X0 - 2, UI_BAR_Y0 - 2, UI_BAR_X1 + 2, UI_BAR_Y0 - 1, LCD_WHITE);
    LCD_Fill(UI_BAR_X0 - 2, UI_BAR_Y1,     UI_BAR_X1 + 2, UI_BAR_Y1 + 1, LCD_WHITE);
    LCD_Fill(UI_BAR_X0 - 2, UI_BAR_Y0 - 2, UI_BAR_X0 - 1, UI_BAR_Y1 + 1, LCD_WHITE);
    LCD_Fill(UI_BAR_X1,     UI_BAR_Y0 - 2, UI_BAR_X1 + 1, UI_BAR_Y1 + 1, LCD_WHITE);
}

/**
 * @brief  绘制烧录进度条。
 * @param  pct   百分比(0~100)
 * @param  color 填充颜色
 */
static void flash_store_ui_draw_pct(uint8_t pct, uint16_t color)
{
    LCD_Fill(60, 42, 180, 80, LCD_BLACK);
    LCD_CS_CLR;
    if (pct >= 100) {
        sok_draw_digit_16x32(88,  44, 1, color);
        sok_draw_digit_16x32(104, 44, 0, color);
        sok_draw_digit_16x32(120, 44, 0, color);
        flash_store_draw_percent(140, 44, color);
    } else if (pct >= 10) {
        sok_draw_digit_16x32(96,  44, pct / 10, color);
        sok_draw_digit_16x32(112, 44, pct % 10, color);
        flash_store_draw_percent(132, 44, color);
    } else {
        sok_draw_digit_16x32(104, 44, pct, color);
        flash_store_draw_percent(124, 44, color);
    }
    LCD_CS_SET;
}

/**
 * @brief  更新烧录进度显示。
 * @param  pct 百分比(0~100)
 */
static void flash_store_ui_update(uint8_t pct)
{
    if (pct > 100) pct = 100;
    if (pct == s_ui_last_pct) return;

    /* 1. 进度条填充(仅补画新增部分) */
    uint16_t fill_w = (uint16_t)pct * UI_BAR_WIDTH / 100;
    if (fill_w > s_ui_last_fill) {
        LCD_Fill(UI_BAR_X0 + s_ui_last_fill, UI_BAR_Y0,
                 UI_BAR_X0 + fill_w, UI_BAR_Y1, LCD_GREEN);
        s_ui_last_fill = fill_w;
    }

    /* 2. 百分比数值 */
    flash_store_ui_draw_pct(pct, LCD_WHITE);
    s_ui_last_pct = pct;
}

/**
 * @brief  烧录完成 UI 提示。
 */
static void flash_store_ui_done(void)
{
    flash_store_ui_draw_pct(100, LCD_GREEN);
}

/**
 * @brief  烧录失败 UI 提示。
 * @param  pct 失败时百分比
 */
static void flash_store_ui_fail(uint8_t pct)
{
    LCD_Fill(UI_BAR_X0, UI_BAR_Y0, UI_BAR_X1, UI_BAR_Y1, LCD_RED);
    flash_store_ui_draw_pct(pct, LCD_RED);
}
#endif /* FLASH_DATA_STORE_MODE || VIDEO_CHUNK_STORE_MODE || BTN_IMG_STORE_MODE */

/* ================================================================
 * 烧录主流程 (FLASH_DATA_STORE_MODE: 字库/图片资源烧录)
 * ================================================================ */
#ifdef FLASH_DATA_STORE_MODE
/* ---- SWD 回读握手/进度 (给 PC 端读, 替代"人眼看屏幕是否 100%") ----
   PC 端用 nm 从 SPI_LCD.elf 查 g_res_prog 的地址, 再用一次 mem2array 把 6 个字读回去。
   ★ magic 必须在最前面且最先写: MCU 刚复位时 .bss 还没清零, PC 可能读到随机 RAM,
     先校验 magic, 对不上就继续等。
   ★ 字库模式烧完是 while(1) 常驻、背光不灭、不进待机 → SWD 一直活着, PC 直接轮询即可。 */
#define RES_PROG_MAGIC   0x52455331u   /* 'R','E','S','1' */

typedef struct {
    volatile uint32_t magic;    /* RES_PROG_MAGIC: 结构有效标志(最先写) */
    volatile uint32_t total;    /* 资源总项数(= FLASH_STORE_RES_COUNT) */
    volatile uint32_t index;    /* 正在烧第几项(1 起), 0=还没开始写 */
    volatile uint32_t pct;      /* 0~100 进度(与屏幕进度条同源) */
    volatile uint32_t crc_fail; /* 回读校验失败的项数 */
    volatile uint32_t result;   /* 0=未完成 1=全部通过 2=有失败 */
} res_prog_t;

volatile res_prog_t g_res_prog = { 0, 0, 0, 0, 0, 0 };

/**
 * @brief  资源布局计算: 补全关卡地图长度 + 累加全部资源总大小
 * @note   关卡地图(sokoban_maps.c 里 50 个独立数组)编译期算不出长度, 只能按
 *         sokoban_levels[i] 的行列推: 长度 = ceil(行*列/2)(Bcd 每字节 2 格)。
 *         ★ 顺序前提: s_resources[] 末尾 50 项 == sok_map1..50 == sokoban_levels[0..49]
 *         ★ 烧"小块"(按钮图/菜单字模)的 BTN_IMG_STORE_MODE 不进这个表(省 50KB 资源),
 *           它改从装置上已有的目录头里读 data_start(FlashStore_ResInit) —— 两边地址一致。
 */
static uint32_t flash_store_layout_total(void)
{
    uint32_t i;
    uint32_t total = 0;

    for (i = 0; i < SOKOBAN_LEVEL_COUNT; i++) {
        s_resources[FLASH_STORE_ID_SOK_MAP_0 + i].size =
            ((uint16_t)sokoban_levels[i].rows * sokoban_levels[i].cols + 1) / 2;
    }
    for (i = 0; i < FLASH_STORE_RES_COUNT; i++) {
        total += s_resources[i].size;
    }
    return total;
}

/**
 * @brief  烧录全部资源到外部 Flash(带进度 UI)。
 */
void FlashStore_BurnAll(void)
{
    uint32_t i;

    /* 0. 识别芯片容量与地址模式(8MB芯片用3字节地址, >16MB用4字节) */
    flash_store_detect_chip();

    /* 0.5 回读握手: magic 最先写, PC 才不会把随机 RAM 当有效数据 */
    g_res_prog.magic    = RES_PROG_MAGIC;
    g_res_prog.total    = FLASH_STORE_RES_COUNT;
    g_res_prog.index    = 0;
    g_res_prog.pct      = 0;
    g_res_prog.crc_fail = 0;
    g_res_prog.result   = 0;

    /* 1. 计算全部资源总大小(含运行期补上的关卡地图长度) */
    uint32_t total = flash_store_layout_total();

    /* 2. 从实际Flash末尾往前布局:
     *    目录区位于最末尾, 数据区紧随其前(从前往后顺序写入) */
    uint32_t dir_start  = s_flash_size - FLASH_STORE_DIR_SIZE;
    uint32_t data_start = dir_start - total;

    /* 3. 初始化显示 */
    flash_store_ui_init();
    flash_store_ui_update(0);

    /* 4. 擦除覆盖区域(按实际地址模式, 4K扇区) */
    {
        uint32_t erase_start = (data_start / 4096) * 4096;
        uint32_t erase_total = s_flash_size - erase_start;
        uint32_t addr;
        for (addr = erase_start; addr < s_flash_size; addr += 4096) {
            flash_store_erase_sector(addr);
            uint8_t p = (uint8_t)(10UL * (addr - erase_start + 4096) / erase_total);
            flash_store_ui_update(p);
            g_res_prog.pct = p;              /* 回读握手: 擦除阶段 0~10 */
        }
    }

    /* 5. 从前往后写入各资源, 同时构建目录 */
    {
        uint8_t *pe = s_dir_buf;
        pe[0] = FLASH_STORE_DIR_MAGIC0;
        pe[1] = FLASH_STORE_DIR_MAGIC1;
        pe[2] = FLASH_STORE_DIR_VERSION;
        pe[3] = (uint8_t)FLASH_STORE_RES_COUNT;
        pe[4] = (uint8_t)(data_start >> 24);
        pe[5] = (uint8_t)(data_start >> 16);
        pe[6] = (uint8_t)(data_start >> 8);
        pe[7] = (uint8_t)data_start;
        pe += FLASH_STORE_DIR_HEADER_SIZE;

        uint32_t cur     = data_start;
        uint32_t written = 0;
        for (i = 0; i < FLASH_STORE_RES_COUNT; i++) {
            const uint8_t *src  = (const uint8_t *)s_resources[i].data;
            uint32_t       size = s_resources[i].size;

            flash_store_write(src, cur, size);
            uint16_t crc = flash_store_crc16(src, size);

            /* 目录项 */
            uint32_t offset = cur - data_start;
            pe[0]  = (uint8_t)i;
            pe[1]  = 0;
            pe[2]  = (uint8_t)(crc >> 8);
            pe[3]  = (uint8_t)crc;
            pe[4]  = (uint8_t)(offset >> 24);
            pe[5]  = (uint8_t)(offset >> 16);
            pe[6]  = (uint8_t)(offset >> 8);
            pe[7]  = (uint8_t)offset;
            pe[8]  = (uint8_t)(size >> 24);
            pe[9]  = (uint8_t)(size >> 16);
            pe[10] = (uint8_t)(size >> 8);
            pe[11] = (uint8_t)size;
            pe += FLASH_STORE_ENTRY_SIZE;

            cur     += size;
            written += size;
            uint8_t p = (uint8_t)(10 + 80UL * written / total);
            flash_store_ui_update(p);
            g_res_prog.index = i + 1;    /* 回读握手: 正在烧第几项(1 起) */
            g_res_prog.pct   = p;        /* 回读握手: 写入阶段 10~90 */
        }

        /* 6. 写入目录区 */
        flash_store_write(s_dir_buf, dir_start, FLASH_STORE_DIR_SIZE);
        flash_store_ui_update(90);
    }

    /* 7. 回读校验: 逐资源比对CRC16 */
    {
        uint8_t ok  = 1;
        uint8_t rbuf[64];
        uint32_t addr = data_start;
        for (i = 0; i < FLASH_STORE_RES_COUNT; i++) {
            uint32_t size = s_resources[i].size;
            uint32_t left = size;
            uint32_t a    = addr;
            uint16_t crc  = 0xFFFF;
            while (left) {
                uint16_t chunk = (left > sizeof(rbuf)) ? (uint16_t)sizeof(rbuf)
                                                       : (uint16_t)left;
                flash_store_read(rbuf, a, chunk);
                for (uint16_t j = 0; j < chunk; j++) {
                    crc = flash_store_crc16_update(crc, rbuf[j]);
                }
                a += chunk;
                left -= chunk;
            }
            if (crc != flash_store_crc16((const uint8_t *)s_resources[i].data, size)) {
                ok = 0;
                g_res_prog.crc_fail++;   /* 回读握手: 失败项计数 */
            }
            addr += size;
            flash_store_ui_update((uint8_t)(90 + 10UL * (i + 1) / FLASH_STORE_RES_COUNT));
            g_res_prog.pct = (uint8_t)(90 + 10UL * (i + 1) / FLASH_STORE_RES_COUNT);
        }
        g_res_prog.pct    = 100;
        g_res_prog.result = ok ? 1 : 2;  /* 回读握手: 1=全部通过 2=有失败 */

        /* 8. 完成显示 */
        if (ok) {
            flash_store_ui_update(100);
            flash_store_ui_done(); /* 绿色100% */
        } else {
            flash_store_ui_fail(s_ui_last_pct); /* 红色进度条+红色百分比提示校验失败 */
        }
    }

    /* 烧录/校验完成后保持画面显示 */
    while (1) {
        Delay_Ms(1000);
    }
}
#endif /* FLASH_DATA_STORE_MODE */

/* ================================================================
 * 视频数据块烧录 (VIDEO_CHUNK_STORE_MODE)
 * 嵌入固件的数据块(video_chunk.c)烧录到外部Flash地址0(视频区域)偏移处,
 * 块大小/偏移由 gen_video_chunk.py 生成的 video_chunk.h 指定(4KB对齐);
 * 屏幕显示百分比: 擦除阶段0-10%, 写入阶段10-90%, 回读校验90-100%。
 * ================================================================ */
#ifdef VIDEO_CHUNK_STORE_MODE
#include "video_chunk.h"

/* ---- SWD 回读握手/进度 (给 PC 端 burn_all.ps1 读, 替代"猜一个固定等待时间") ----
   PC 端用 nm 从 SPI_LCD.elf 查这个符号的地址, 再用一次 mem2array 把 10 个字读回去;
   用结构体是为了保证这些字连续, 一次读就能拿到 magic/块号/阶段/进度/两个CRC/结果/三段耗时。
   ★ magic 必须在最前面且最先写: 合并会话后 PC 可能在 MCU 刚复位、.bss 还没清零时就读,
     读到的是随机 RAM; PC 先校验 magic, 对不上就继续等, 免得把随机数当"烧完了"。 */
#define BURN_PROG_MAGIC   0x4255524Eu   /* 'B','U','R','N' */

typedef struct {
    volatile uint32_t magic;    /* BURN_PROG_MAGIC: 结构有效标志(最先写) */
    volatile uint32_t index;    /* 块号(VIDEO_CHUNK_INDEX) */
    volatile uint32_t stage;    /* 0=擦除 1=写入 2=回读校验 3=结束(通过或失败) */
    volatile uint32_t pct;      /* 0~100 当前进度(与屏幕进度条同源) */
    volatile uint32_t crc_src;  /* 源数据 CRC16 */
    volatile uint32_t crc_rd;   /* 回读数据 CRC16(失败时 PC 日志里可直接对比) */
    volatile uint32_t result;   /* 0=未完成 1=校验通过 2=校验失败 */
    volatile uint32_t ms_erase; /* 擦除阶段耗时(ms), 供评估"是否值得改用块擦除" */
    volatile uint32_t ms_write; /* 写入阶段耗时(ms) */
    volatile uint32_t ms_verify;/* 回读校验阶段耗时(ms) */
} burn_prog_t;

volatile burn_prog_t g_burn_prog = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

/* ---- 分阶段计时: 借 TIM1 当自由计数器(144MHz/14400 = 10kHz → 0.1ms/tick, 6.55s 回绕一次) ----
   不使能中断 —— 免得把主程序 TIM1 更新中断里的电池检测拉进烧录模式。
   背光 PWM 用的是 TIM4, 游戏模式的帧计时器在烧录模式里根本没初始化, 所以这里独占 TIM1 ? */
static void burn_timer_init(void)
{
    TIM_TimeBaseInitTypeDef t = {0};
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_TIM1, ENABLE);
    t.TIM_Period        = 65535;
    t.TIM_Prescaler     = 14399;   /* 144MHz / (14399+1) = 10kHz */
    t.TIM_ClockDivision = TIM_CKD_DIV1;
    t.TIM_CounterMode   = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM1, &t);
    TIM_Cmd(TIM1, ENABLE);
}

static uint16_t burn_tick(void)
{
    return (uint16_t)TIM1->CNT;
}

/* 距 t0 过了多少 ms(0.1ms/tick, 只适用 <6.5s 的间隔) */
static uint32_t burn_ms_since(uint16_t t0)
{
    return (uint32_t)((uint16_t)(burn_tick() - t0)) / 10;
}

/**
 * @brief  视频数据块烧录主流程 (不返回)
 * @note   进入前需已初始化SPI Flash与LCD;
 *         将嵌入固件的 g_video_chunk[VIDEO_CHUNK_SIZE] 从外部Flash的
 *         VIDEO_CHUNK_OFFSET 开始顺序写入(扇区擦除+写入), 完成后回读CRC校验
 */
void VideoChunkStore_BurnAll(void)
{
    g_burn_prog.magic = BURN_PROG_MAGIC;     /* 回读握手: 先置有效标志, PC 才认后面的字段 */
    flash_store_detect_chip();   /* 识别芯片容量与地址模式 */
    g_burn_prog.index = VIDEO_CHUNK_INDEX;   /* 回读握手: 告诉 PC 本块是第几块 */
    burn_timer_init();                       /* 分段计时用(TIM1 自由计数) */
    uint16_t t_phase = burn_tick();

    flash_store_ui_init();
    flash_store_ui_update(0);

    /* 显示剩余烧录次数(当前块之后的块数, 供观察烧录进度) */
    LCD_ShowNum((uint16_t)(VIDEO_BLOCK_COUNT - VIDEO_CHUNK_INDEX - 1), 175);

    /* 1. 边界检查: 本块不得超出视频区域(Flash容量内) */
    uint32_t start = VIDEO_CHUNK_OFFSET;
    uint32_t end   = start + VIDEO_CHUNK_SIZE;
    if (end > s_flash_size) {
        flash_store_ui_fail(0);
        g_burn_prog.stage  = 3;
        g_burn_prog.result = 2;   /* 握手: 本块越界, 直接报失败(不让 PC 白等超时) */
        while (1) { Delay_Ms(1000); }
    }

    /* 2. 擦除本块覆盖的全部扇区(4KB对齐) */
    {
        uint32_t erase_total = ((end + 4095) & ~((uint32_t)4095)) -
                               (start & ~((uint32_t)4095));
        uint32_t done = 0;
        uint32_t addr;
        t_phase = burn_tick();
        g_burn_prog.stage = 0;
        for (addr = start & ~((uint32_t)4095); addr < end; addr += 4096) {
            flash_store_erase_sector(addr);
            done += 4096;
            uint8_t p = (uint8_t)(10UL * done / erase_total);
            flash_store_ui_update(p);
            g_burn_prog.pct = p;
        }
        g_burn_prog.ms_erase = burn_ms_since(t_phase);
    }

    /* 3. 写入数据(每次1个扇区大小, 便于进度更新) */
    {
        uint32_t written = 0;
        t_phase = burn_tick();
        g_burn_prog.stage = 1;
        while (written < VIDEO_CHUNK_SIZE) {
            uint32_t n = VIDEO_CHUNK_SIZE - written;
            if (n > 4096) n = 4096;
            flash_store_write(g_video_chunk + written, start + written, n);
            written += n;
            uint8_t p = (uint8_t)(10 + 80UL * written / VIDEO_CHUNK_SIZE);
            flash_store_ui_update(p);
            g_burn_prog.pct = p;
        }
        g_burn_prog.ms_write = burn_ms_since(t_phase);
    }

    /* 4. 回读校验: 与源数据CRC比对 */
    {
        uint8_t rbuf[64];
        uint32_t left = VIDEO_CHUNK_SIZE;
        uint32_t a    = start;
        uint16_t c    = 0xFFFF;
        t_phase = burn_tick();
        g_burn_prog.stage = 2;
        while (left) {
            uint16_t n = (left > sizeof(rbuf)) ? (uint16_t)sizeof(rbuf)
                                               : (uint16_t)left;
            flash_store_read(rbuf, a, n);
            for (uint16_t j = 0; j < n; j++) {
                c = flash_store_crc16_update(c, rbuf[j]);
            }
            a += n;
            left -= n;
            uint8_t p = (uint8_t)(90 + 10UL * (VIDEO_CHUNK_SIZE - left) / VIDEO_CHUNK_SIZE);
            flash_store_ui_update(p);
            g_burn_prog.crc_rd = c;
            g_burn_prog.pct    = p;
        }
        uint16_t c_src = flash_store_crc16(g_video_chunk, VIDEO_CHUNK_SIZE);
        g_burn_prog.ms_verify = burn_ms_since(t_phase);
        g_burn_prog.crc_src = c_src;
        g_burn_prog.stage   = 3;
        if (c == c_src) {
            flash_store_ui_update(100);
            flash_store_ui_done();     /* 绿色100% */
            g_burn_prog.result = 1;    /* 握手: 烧完且校验通过 */
        } else {
            flash_store_ui_fail(s_ui_last_pct);  /* 红色 */
            g_burn_prog.result = 2;    /* 握手: 校验失败 */
        }
    }

    /* 烧录/校验完成后保持画面显示 */
    while (1) {
        Delay_Ms(1000);
    }
}
#endif /* VIDEO_CHUNK_STORE_MODE */

/* ================================================================
 * 英文版推箱子按钮图 + 菜单"游戏名"汉字字模 烧录 (BTN_IMG_STORE_MODE)
 * ----------------------------------------------------------------
 * 只写外部 Flash 的固定保留区 BTN_IMG_EN_ADDR:
 *   [3 张英文按钮图 5580B][10 个 24x24 汉字字模 720B] = 6300B
 * 不碰字库资源区、不碰目录 → 加/换这两样都不需要重烧字库。
 * 进度: 擦除 0~10%、写入 10~90%、回读校验 90~100%, 完成后常驻 100%。
 * ================================================================ */
#ifdef BTN_IMG_STORE_MODE
#include "sok_img_prev.h"      /* g_btn_img_en[]: 三张英文按钮图连在一起 */
#include "chinese_fonts.h"     /* g_menu_glyph_ext[]: 菜单游戏名用的 10 个汉字 */

#define BTN_PROG_MAGIC   0x42544E31u   /* 'B','T','N','1' */

typedef struct {
    volatile uint32_t magic;    /* BTN_PROG_MAGIC */
    volatile uint32_t size;     /* 本区字节数(= BTN_IMG_EN_SIZE) */
    volatile uint32_t pct;      /* 0~100 */
    volatile uint32_t result;   /* 0=未完成 1=校验通过 2=校验失败 */
} btn_prog_t;

volatile btn_prog_t g_btn_prog = { 0, 0, 0, 0 };

/**
 * @brief  烧录英文按钮图(固定保留区) + 菜单游戏名汉字(字库区紧前面) (不返回)
 * @note   两小块都不进资源表/目录, 所以改它们不用重烧字库:
 *         1) 英文按钮图 5580B → BTN_IMG_EN_ADDR (视频区与字库区之间的空闲保留区)
 *         2) 菜单游戏名 10 个汉字 720B → 字库数据区紧前面的扇区尾部
 *            (与字库同一个区, 但落在"全量烧字库"的擦除范围之外 → 烧字库擦不到它)
 */
void BtnImgStore_BurnAll(void)
{
    uint32_t glyph_addr = 0;
    uint8_t  have_glyph;

    flash_store_detect_chip();

    /* 字模那一小块紧贴在字库数据区前面 → 地址从装置上**已有字库的目录头**里读 data_start
       (与游戏模式共用 flash_store_glyph_addr_from, 两边地址必然一致)。
       目录无效 = 还没烧过字库 → 只烧按钮图、跳过字模(这种板子本来就要先全量烧字库)。 */
    have_glyph = FlashStore_ResInit() ? 1 : 0;
    if (have_glyph) glyph_addr = flash_store_glyph_addr_from(s_data_start);

    g_btn_prog.magic  = BTN_PROG_MAGIC;
    g_btn_prog.size   = (uint32_t)BTN_IMG_EN_SIZE + (have_glyph ? (uint32_t)MENU_GLYPH_SIZE : 0UL);
    g_btn_prog.pct    = 0;
    g_btn_prog.result = 0;

    flash_store_ui_init();
    flash_store_ui_update(0);

    /* 1. 擦除: 按钮保留区覆盖的扇区(2个) + 字模那一小块所在扇区(在字库区之前, 独立扇区) */
    {
        uint32_t start = BTN_IMG_EN_ADDR & ~(uint32_t)4095;
        uint32_t end   = (BTN_IMG_EN_ADDR + BTN_IMG_EN_SIZE + 4095u) & ~(uint32_t)4095;
        uint32_t addr;
        for (addr = start; addr < end; addr += 4096) {
            flash_store_erase_sector(addr);
        }
        if (have_glyph) flash_store_erase_sector(glyph_addr & ~(uint32_t)4095);
        g_btn_prog.pct = 10;
        flash_store_ui_update(10);
    }

    /* 2. 写入 3 张英文按钮图 */
    {
        uint32_t written = 0;
        while (written < BTN_IMG_EN_SIZE) {
            uint32_t n = BTN_IMG_EN_SIZE - written;
            if (n > 4096) n = 4096;
            flash_store_write(g_btn_img_en + written, BTN_IMG_EN_ADDR + written, n);
            written += n;
            g_btn_prog.pct = (uint8_t)(10 + 60UL * written / BTN_IMG_EN_SIZE);
            flash_store_ui_update((uint8_t)g_btn_prog.pct);
        }
    }

    /* 3. 写入菜单游戏名汉字 (720B) */
    if (have_glyph) {
        uint32_t written = 0;
        while (written < MENU_GLYPH_SIZE) {
            uint32_t n = MENU_GLYPH_SIZE - written;
            flash_store_write(g_menu_glyph_ext + written, glyph_addr + written, n);
            written += n;
            g_btn_prog.pct = (uint8_t)(70 + 15UL * written / MENU_GLYPH_SIZE);
            flash_store_ui_update((uint8_t)g_btn_prog.pct);
        }
    }

    /* 4. 回读校验: 两小块分别与固件里的源数据比 CRC16 */
    {
        uint8_t  rbuf[64];
        uint16_t c1 = 0xFFFF, c2 = 0xFFFF;
        uint16_t s1, s2 = 0xFFFF;
        uint32_t i;
        uint8_t  ok;

        for (i = 0; i < BTN_IMG_EN_SIZE; i += (uint32_t)sizeof(rbuf)) {
            uint32_t left = BTN_IMG_EN_SIZE - i;
            uint16_t n = (left > sizeof(rbuf)) ? (uint16_t)sizeof(rbuf) : (uint16_t)left;
            uint16_t j;
            flash_store_read(rbuf, BTN_IMG_EN_ADDR + i, n);
            for (j = 0; j < n; j++) c1 = flash_store_crc16_update(c1, rbuf[j]);
            g_btn_prog.pct = (uint8_t)(85 + 8UL * i / BTN_IMG_EN_SIZE);
            flash_store_ui_update((uint8_t)g_btn_prog.pct);
        }
        if (have_glyph) {
            for (i = 0; i < MENU_GLYPH_SIZE; i += (uint32_t)sizeof(rbuf)) {
                uint32_t left = MENU_GLYPH_SIZE - i;
                uint16_t n = (left > sizeof(rbuf)) ? (uint16_t)sizeof(rbuf) : (uint16_t)left;
                uint16_t j;
                flash_store_read(rbuf, glyph_addr + i, n);
                for (j = 0; j < n; j++) c2 = flash_store_crc16_update(c2, rbuf[j]);
            }
            s2 = flash_store_crc16(g_menu_glyph_ext, MENU_GLYPH_SIZE);
        }
        s1 = flash_store_crc16(g_btn_img_en, BTN_IMG_EN_SIZE);
        ok = ((c1 == s1) && (c2 == s2)) ? 1 : 0;

        g_btn_prog.pct    = 100;
        g_btn_prog.result = ok ? 1 : 2;
        if (ok) {
            flash_store_ui_update(100);
            flash_store_ui_done();
        } else {
            flash_store_ui_fail(s_ui_last_pct);
        }
    }

    while (1) {
        Delay_Ms(1000);
    }
}
#endif /* BTN_IMG_STORE_MODE */
