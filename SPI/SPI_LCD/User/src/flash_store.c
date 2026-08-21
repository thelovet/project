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
#include "debug.h"

#include "chinese_fonts.h"   /* 24x24中文点阵字库 */
#include "sok_img_win.h"     /* 32x32中文字库 + 16x32数字字库 */
#include "sokoban.h"         /* 推箱子图像数据 */
#include "sok_img_prev.h"    /* 上一关图标 */
#include "sok_img_next.h"    /* 下一关图标 */
#include "sok_img_restart.h" /* 重新开始图标 */

/* ================================================================
 * 资源表: 固件内全部需要烧录的字库与图片数据
 * ================================================================ */
typedef struct {
    const void *data;   /* 数据源指针(内部Flash const区) */
    uint32_t    size;   /* 数据长度(字节) */
} FlashStoreRes;

static const FlashStoreRes s_resources[] = {
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
    /* ---- 导航图标 (30x31 RGB565) ---- */
    { sok_img_prev,     PREV_IMG_W * PREV_IMG_H * 2 },
    { sok_img_next,     NEXT_IMG_W * NEXT_IMG_H * 2 },
    { sok_img_restart,  RESTART_IMG_W * RESTART_IMG_H * 2 },
};

#define FLASH_STORE_RES_COUNT \
    (sizeof(s_resources) / sizeof(s_resources[0]))

/* 资源表数量必须与flash_store.h中定义的资源ID数量一致(编译期检查) */
typedef char flash_store_count_check[
    ((FLASH_STORE_ID_COUNT == FLASH_STORE_RES_COUNT) ? 1 : -1)];

/* 目录缓冲(位于Flash最末尾) */
#define FLASH_STORE_DIR_SIZE \
    (FLASH_STORE_DIR_HEADER_SIZE + FLASH_STORE_RES_COUNT * FLASH_STORE_ENTRY_SIZE)
static uint8_t s_dir_buf[FLASH_STORE_DIR_SIZE];

/* 运行期共享scratch缓冲区(最大资源: 导航图标1860字节) */
#define FLASH_STORE_SCRATCH_SIZE  2048
static uint8_t s_scratch[FLASH_STORE_SCRATCH_SIZE];

/* ================================================================
 * CRC16 (MODBUS: 多项式0x8005反向0xA001, 初值0xFFFF)
 * ================================================================ */
static uint16_t flash_store_crc16_update(uint16_t crc, uint8_t byte)
{
    crc ^= byte;
    for (uint8_t i = 0; i < 8; i++) {
        if (crc & 1) crc = (crc >> 1) ^ 0xA001;
        else         crc >>= 1;
    }
    return crc;
}

static uint16_t flash_store_crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFF;
    while (len--) crc = flash_store_crc16_update(crc, *data++);
    return crc;
}

/* ================================================================
 * 4字节地址SPI Flash访问 (兼容 >16MB 地址区域, 不改变芯片地址模式)
 * ================================================================ */

/* 扇区擦除(4K) */
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

/* 页编程(单页≤256字节, 不能跨页) */
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

/* 跨页写 */
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

/* 读数据(4字节地址) */
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

/* 根据JEDEC ID容量字节识别芯片大小 */
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

/* 统一扇区擦除(按实际地址模式) */
static void flash_store_erase_sector(uint32_t addr)
{
    if (s_use_4b) {
        flash_store_erase_sector_4b(addr);
    } else {
        SPI_Flash_Erase_Sector(addr / 4096);   /* 3字节地址, 参数为扇区序号 */
    }
}

/* 统一跨页写(按实际地址模式) */
static void flash_store_write(const uint8_t *src, uint32_t addr, uint32_t len)
{
    if (s_use_4b) {
        flash_store_write_4b(src, addr, len);
    } else {
        SPI_Flash_Write_NoCheck((uint8_t *)src, addr, (uint16_t)len);
    }
}

/* 统一读数据(按实际地址模式) */
static void flash_store_read(uint8_t *buf, uint32_t addr, uint16_t len)
{
    if (s_use_4b) {
        flash_store_read_4b(buf, addr, len);
    } else {
        SPI_Flash_Read(buf, addr, len);
    }
}

/* ================================================================
 * 运行期资源读取 (游戏模式: 从外部Flash按ID读取已烧录的字库/图片)
 * ================================================================ */
static uint8_t  s_res_valid = 0;     /* 目录缓存是否有效 */
static uint32_t s_data_start = 0;    /* 数据区起始地址(来自目录头) */

/* 目录项解析辅助: 读取条目中的4字节大端字段 */
static uint32_t flash_store_dir_field(const uint8_t *e, uint8_t off)
{
    return ((uint32_t)e[off] << 24) | ((uint32_t)e[off + 1] << 16) |
           ((uint32_t)e[off + 2] << 8) | (uint32_t)e[off + 3];
}

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

uint8_t *FlashStore_Scratch(void)
{
    return s_scratch;
}

/* ================================================================
 * 进度条UI (240x240屏幕)
 * ================================================================ */
#define UI_BAR_X0      17   /* 进度条填充区左边界 */
#define UI_BAR_X1      225  /* 进度条填充区右边界(不含) */
#define UI_BAR_Y0      92   /* 进度条填充区上边界 */
#define UI_BAR_Y1      150  /* 进度条填充区下边界(不含) */
#define UI_BAR_WIDTH   (UI_BAR_X1 - UI_BAR_X0)

static uint8_t  s_ui_last_pct  = 0xFF; /* 上次绘制百分比(仅变化时重绘) */
static uint16_t s_ui_last_fill = 0;    /* 上次已填充宽度 */

/* 绘制%符号: 两个圆点+斜线 */
static void flash_store_draw_percent(uint16_t x, uint16_t y, uint16_t color)
{
    LCD_Fill(x,     y,      x + 4, y + 4, color); /* 右上圆点 */
    LCD_Fill(x + 2, y + 14, x + 6, y + 18, color); /* 左下圆点 */
    for (uint8_t i = 0; i < 7; i++) {              /* 斜线 */
        LCD_Fill(x + 1 + i * 2, y + 3 + i * 2, x + 3 + i * 2, y + 5 + i * 2, color);
    }
}

/* 初始化UI: 清屏 + 画进度条边框 */
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

/* 绘制百分比数字(居中, 指定颜色) */
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

/* 更新进度条填充与百分比数值 */
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

/* 烧录完成: 绿色突出显示100% */
static void flash_store_ui_done(void)
{
    flash_store_ui_draw_pct(100, LCD_GREEN);
}

/* 校验失败: 进度条变红, 百分比数字变红提示 */
static void flash_store_ui_fail(uint8_t pct)
{
    LCD_Fill(UI_BAR_X0, UI_BAR_Y0, UI_BAR_X1, UI_BAR_Y1, LCD_RED);
    flash_store_ui_draw_pct(pct, LCD_RED);
}

/* ================================================================
 * 烧录主流程
 * ================================================================ */
void FlashStore_BurnAll(void)
{
    uint32_t i;

    /* 0. 识别芯片容量与地址模式(8MB芯片用3字节地址, >16MB用4字节) */
    flash_store_detect_chip();

    /* 1. 计算全部资源总大小 */
    uint32_t total = 0;
    for (i = 0; i < FLASH_STORE_RES_COUNT; i++) {
        total += s_resources[i].size;
    }

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
            flash_store_ui_update(
                (uint8_t)(10UL * (addr - erase_start + 4096) / erase_total));
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
            flash_store_ui_update((uint8_t)(10 + 80UL * written / total));
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
            }
            addr += size;
            flash_store_ui_update((uint8_t)(90 + 10UL * (i + 1) / FLASH_STORE_RES_COUNT));
        }

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
