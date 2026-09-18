/*********************************************************************
 * File Name          : flash_store.h
 * Description        : Flash数据存储程序头文件
 *                      将固件内所有字库资源与图片数据烧录到外部SPI Flash。
 *                      存储区域从64MB Flash的末尾地址往前排列:
 *                      先计算全部资源的总大小, 再从后往前布局,
 *                      数据区从"末尾总大小"起始地址开始顺序写入(从前往后),
 *                      目录区存放在Flash的最末尾。
 ********************************************************************/
#ifndef __FLASH_STORE_H__
#define __FLASH_STORE_H__

#include <stdint.h>

/* ================================================================
 * 配置
 * ================================================================ */
/* 外部SPI Flash总容量: 64MB (W25Q512, 需4字节地址访问) */
#define FLASH_STORE_FLASH_SIZE      (64UL * 1024UL * 1024UL)

/* ================================================================
 * 目录区格式 (存放在Flash最末尾)
 *  头部(8字节): 魔数2 + 版本1 + 资源数量1 + 数据起始地址4
 *  目录项(12字节): id1 + 保留1 + CRC16(高字节在前)2 + 相对偏移4 + 长度4
 * ================================================================ */
#define FLASH_STORE_DIR_MAGIC0      0xA5
#define FLASH_STORE_DIR_MAGIC1      0x5A
#define FLASH_STORE_DIR_VERSION     1
#define FLASH_STORE_DIR_HEADER_SIZE 8
#define FLASH_STORE_ENTRY_SIZE      12

/* ================================================================
 * 资源ID (与flash_store.c中资源表顺序一一对应)
 * ================================================================ */
typedef enum {
    /* 0-21: 24x24中文点阵字库 (chinese_fonts.c) */
    FLASH_STORE_ID_24_TUI = 0,     /* 推 */
    FLASH_STORE_ID_24_XIANG,       /* 箱 */
    FLASH_STORE_ID_24_ZI,          /* 子 */
    FLASH_STORE_ID_24_SHI,         /* 视 */
    FLASH_STORE_ID_24_PIN,         /* 频 */
    FLASH_STORE_ID_24_FAN,         /* 返 */
    FLASH_STORE_ID_24_CHONG,       /* 重 */
    FLASH_STORE_ID_24_XIN,         /* 新 */
    FLASH_STORE_ID_24_KAI,         /* 开 */
    FLASH_STORE_ID_24_SHI3,        /* 始 */
    FLASH_STORE_ID_24_HUI,         /* 回 */
    FLASH_STORE_ID_24_LIANG,       /* 亮 */
    FLASH_STORE_ID_24_DU,          /* 度 */
    FLASH_STORE_ID_24_SHE,         /* 设 */
    FLASH_STORE_ID_24_ZHI,         /* 置 */
    FLASH_STORE_ID_24_XI2,         /* 息 */
    FLASH_STORE_ID_24_PING,        /* 屏 */
    FLASH_STORE_ID_24_SHI2,        /* 时 */
    FLASH_STORE_ID_24_JIAN,        /* 间 */
    FLASH_STORE_ID_24_MU,          /* 幕 */
    FLASH_STORE_ID_24_FAN2,        /* 反 */
    FLASH_STORE_ID_24_XIANG2,      /* 向 */

    /* 22-30: 32x32中文字库 (sok_img_win.c) */
    FLASH_STORE_ID_32_GONG = 22,   /* 恭 */
    FLASH_STORE_ID_32_XI,          /* 喜 */
    FLASH_STORE_ID_32_TONG,        /* 通 */
    FLASH_STORE_ID_32_GUAN,        /* 关 */
    FLASH_STORE_ID_32_KA,          /* 卡 */
    FLASH_STORE_ID_32_YOU,         /* 游 */
    FLASH_STORE_ID_32_XI3,         /* 戏 */
    FLASH_STORE_ID_32_JIE,         /* 结 */
    FLASH_STORE_ID_32_SHU,         /* 束 */

    /* 31: 16x32数字字库 */
    FLASH_STORE_ID_DIGITS = 31,

    /* 32-33: 推箱子背景图(调色板+像素) */
    FLASH_STORE_ID_BG_PALETTE = 32,
    FLASH_STORE_ID_BG_IMG,
    /* 34-35: 墙图 */
    FLASH_STORE_ID_WALL_PALETTE = 34,
    FLASH_STORE_ID_WALL_IMG,
    /* 36-37: 箱子图 */
    FLASH_STORE_ID_BOX_PALETTE = 36,
    FLASH_STORE_ID_BOX_IMG,
    /* 38-39: 箱子在目标上图 */
    FLASH_STORE_ID_BOX_ON_TARGET_PALETTE = 38,
    FLASH_STORE_ID_BOX_ON_TARGET_IMG,
    /* 40-41: 目标点图 */
    FLASH_STORE_ID_TARGET_PALETTE = 40,
    FLASH_STORE_ID_TARGET_IMG,
    /* 42: 玩家图 */
    FLASH_STORE_ID_PLAYER_IMG = 42,
    /* 43: 上一关图标 */
    FLASH_STORE_ID_IMG_PREV = 43,
    /* 44: 下一关图标 */
    FLASH_STORE_ID_IMG_NEXT = 44,
    /* 45: 重新开始图标 */
    FLASH_STORE_ID_IMG_RESTART = 45,

    /* 46-55: 大鱼吃小鱼 10条鱼图片 (RGB565 小端序, 白色透明) */
    FLASH_STORE_ID_FISH_0 = 46,
    FLASH_STORE_ID_FISH_1,
    FLASH_STORE_ID_FISH_2,
    FLASH_STORE_ID_FISH_3,
    FLASH_STORE_ID_FISH_4,
    FLASH_STORE_ID_FISH_5,
    FLASH_STORE_ID_FISH_6,
    FLASH_STORE_ID_FISH_7,
    FLASH_STORE_ID_FISH_8,
    FLASH_STORE_ID_FISH_9,

    /* 56-64: 大鱼吃小鱼 9张玩家形象 (RGB565 小端序, 白色透明) */
    FLASH_STORE_ID_PLAYER_0 = 56,
    FLASH_STORE_ID_PLAYER_1,
    FLASH_STORE_ID_PLAYER_2,
    FLASH_STORE_ID_PLAYER_3,
    FLASH_STORE_ID_PLAYER_4,
    FLASH_STORE_ID_PLAYER_5,
    FLASH_STORE_ID_PLAYER_6,
    FLASH_STORE_ID_PLAYER_7,
    FLASH_STORE_ID_PLAYER_8,

    /* 65-114: 50 张推箱子关卡地图(Bcd 压缩, 每字节存 2 格; 顺序 = sokoban_maps.c 的 sok_map1..50)
       ★ 与 s_resources[] 末尾 50 项一一对应; 游戏里用 FLASH_STORE_ID_SOK_MAP_0 + 关卡序号 读取
       ★ 这张表一改, 外部 Flash 资源区布局就变 → 所有板子都得重烧"字库/图片"那一步(烧录脚本会做) */
    FLASH_STORE_ID_SOK_MAP_0 = 65,
    FLASH_STORE_ID_SOK_MAP_LAST = 114,

    /* 115-118: "语言设置"用 24x24 汉字 (chinese_fonts.c)
       ★ 追加在表尾而不是插进前面的 0~21, 以免把已有 ID 全部顶位:
         没重烧字库的旧板子只会"语言设置"这几个字空白, 其它功能照常 */
    FLASH_STORE_ID_24_YU2 = 115,   /* 语 */
    FLASH_STORE_ID_24_YAN,         /* 言 */
    FLASH_STORE_ID_24_ZHONG,       /* 中 */
    FLASH_STORE_ID_24_WEN,         /* 文 */

    /* 119-125: 16x24 半宽拉丁字母, 供"语言设置"子页的 "English" 选项
       (24行 x 2字节 = 48字节/字, MSB在左; 与 24x24 汉字同一套取模方式) */
    FLASH_STORE_ID_16_E = 119,
    FLASH_STORE_ID_16_N,
    FLASH_STORE_ID_16_G,   /* 小写 g (字模 font_lat_g) */
    FLASH_STORE_ID_16_L,
    FLASH_STORE_ID_16_I,
    FLASH_STORE_ID_16_S,
    FLASH_STORE_ID_16_H,

    /* 126-127: English UI glyph sets (one resource per set, read glyph by offset)
       LATIN24 = 27 x 16x24 half-width glyphs (48 B each)
       LATIN32 = 18 x 16x32 half-width glyphs (64 B each) */
    FLASH_STORE_ID_LATIN24 = 126,
    FLASH_STORE_ID_LATIN32,

    FLASH_STORE_ID_COUNT
} FlashStoreId;

/* ================================================================
 * 英文版推箱子导航按钮图的保留区 (不走资源表!)
 * ----------------------------------------------------------------
 * 中文那套在资源表里(ID 71~73), 英文那套另存一块固定地址区域:
 *   - 内部 Flash 放不下两套按钮图(字库固件只剩 460B, 两套要 11,160B),
 *     所以英文这套用独立烧录模式 BTN_IMG_STORE_MODE 单独烧一次;
 *   - 不放进资源表 = 不动目录/不动布局 → 不用重烧整份字库, 也不用担心
 *     "跳过不写的项和邻居共用扇区被擦掉"的问题(这是原方案最大的坑)。
 * 区域地址: 视频区(0 ~ 约2.87M)与字库区(约8.19M起)之间的空闲区, 取 4KB 对齐。
 *   ★ 以后视频长度大幅增加 / 字库区变大时, 要重新确认这块不被占用。
 * 布局: [prev_en][next_en][restart_en], 每张 30x31 RGB565 小端 = 1860 字节。
 * ================================================================ */
#define BTN_IMG_EN_ADDR   0x00600000UL        /* 保留区起始地址(4KB对齐) */
#define BTN_IMG_EN_ONE    1860UL              /* 单张按钮图字节数 (30*31*2) */
#define BTN_IMG_EN_SIZE   (BTN_IMG_EN_ONE * 3UL)   /* 3 张: 上一关/下一关/重新开始 */
#define BTN_IMG_EN_PREV     0UL               /* 区域内偏移 */
#define BTN_IMG_EN_NEXT     BTN_IMG_EN_ONE
#define BTN_IMG_EN_RESTART  (BTN_IMG_EN_ONE * 2UL)

/* ---- 撤销界面的 3 个按钮图 (上一步/返回/下一步, 中英各一套) ----
   和上面英文按钮图走完全相同的路子(固定保留区 + BTN_IMG_STORE_MODE 单独烧):
   ★ 中文这套也**不能**进资源表 —— 字库固件只余 316B, 而一套 3 张 = 5580B。
   布局: [上一步][返回][下一步], 每张 30x31 RGB565 小端 = 1860 字节。
   运行期按 g_language_index 选基地址, 再用下面的偏移取图(见 sokoban.c)。
   两块都是 4KB 对齐, 且 0x602000+5580 < 0x604000, 不会互相覆盖。 */
#define BTN_IMG_UNDO_ONE     1860UL                    /* 单张字节数 (30*31*2) */
#define BTN_IMG_UNDO_SIZE    (BTN_IMG_UNDO_ONE * 3UL)  /* 一套 3 张 = 5580 */
#define BTN_IMG_UNDO_PREV    0UL                       /* 上一步 */
#define BTN_IMG_UNDO_BACK    BTN_IMG_UNDO_ONE          /* 返回 */
#define BTN_IMG_UNDO_NEXT    (BTN_IMG_UNDO_ONE * 2UL)  /* 下一步 */
#define BTN_IMG_UNDO_CN_ADDR 0x00602000UL              /* 中文套保留区(4KB对齐) */
#define BTN_IMG_UNDO_EN_ADDR 0x00604000UL              /* 英文套保留区(4KB对齐) */

/* 菜单"游戏名"用的汉字字模 (见 chinese_fonts.h 的 MENU_EXT_ID_*):
   放在**字库数据区紧前面**(与字库同一个区), 地址 = (data_start 向下取整到 4KB) - MENU_GLYPH_SIZE。
   这样它落在"全量烧字库"的擦除范围(从 data_start 所在扇区起到芯片末尾)之外,
   所以烧字库不会把它擦掉; 它由 BTN_IMG_STORE_MODE 单独烧一次(和英文按钮图一起)。
   ★ 字库资源总量变化(data_start 移动)后, 必须重烧这一块。
   读取接口 FlashStore_MenuGlyphRead() 内部按目录头里的 data_start 现算地址。 */
#define MENU_GLYPH_ONE    72UL                                  /* 单字 24x24 1bpp */
#define MENU_GLYPH_CNT    14UL                                  /* 方块消行/华容道/大鱼小鱼/合并 + 撤销 */
#define MENU_GLYPH_SIZE   (MENU_GLYPH_ONE * MENU_GLYPH_CNT)     /* 1008 字节 */

/* ================================================================
 * 接口
 * ================================================================ */
/**
 * @brief  执行全部资源烧录
 * @note   流程: 计算总大小→从末尾往前布局→擦除→写入数据→写入目录→CRC回读校验
 *         完成后屏幕显示100%, 失败时进度条变红; 本函数不返回。
 */
void FlashStore_BurnAll(void);

/**
 * @brief  英文版按钮图 + 撤销界面按钮图 + 菜单字模 烧录 (BTN_IMG_STORE_MODE)
 * @note   写三小块, 都不碰字库资源区/目录, 所以不用重烧字库:
 *         1) 英文按钮图 5580B → 固定保留区 BTN_IMG_EN_ADDR
 *         2) 撤销按钮图(中英各 5580B) → BTN_IMG_UNDO_CN_ADDR / BTN_IMG_UNDO_EN_ADDR
 *         3) 菜单追加字模(14 个汉字 1008B) → 字库数据区紧前面(见 MENU_GLYPH_SIZE 注释)
 *         擦除+写入+CRC回读校验, 完成常驻显示100%; 本函数不返回。
 */
void BtnImgStore_BurnAll(void);

/**
 * @brief  读取"菜单"追加字模 (14 个 24x24 汉字, 共 MENU_GLYPH_SIZE 字节)
 * @param  buf 目标缓冲(至少 MENU_GLYPH_SIZE 字节)
 * @return 1=读取完成(没烧过时内容是 0xFF, 字不显示, 其它功能不受影响)
 * @note   地址按目录头里的 data_start 现算: (data_start 向下取整 4KB) - MENU_GLYPH_SIZE
 */
uint8_t FlashStore_MenuGlyphRead(uint8_t *buf);

/* ================================================================
 * 运行期资源读取接口 (游戏模式使用)
 * 通过已烧录到外部Flash末尾的目录表, 按资源ID随机读取数据
 * ================================================================ */
/** @brief 初始化资源读取: 从外部Flash末尾读取目录并缓存, 返回1=成功(已烧录) */
uint8_t FlashStore_ResInit(void);

/**
 * @brief  按绝对地址读一段外部 Flash 数据(不走资源目录)
 * @param  addr 外部 Flash 绝对地址
 * @param  buf  目标缓冲区
 * @param  len  读取字节数
 * @return 实际读取字节数(上限 0xFFFF)
 * @note   供"固定保留区"(如 BTN_IMG_EN_ADDR 英文按钮图)使用
 */
uint16_t FlashStore_RawRead(uint32_t addr, uint8_t *buf, uint16_t len);

/**
 * @brief  按资源ID读取一段数据
 * @param  id     资源ID (FlashStoreId)
 * @param  offset 资源内偏移(字节)
 * @param  buf    目标缓冲区
 * @param  len    期望读取字节数
 * @return 实际读取字节数(越界时截断, 未初始化/失败返回0)
 */
uint16_t FlashStore_ResRead(FlashStoreId id, uint32_t offset, uint8_t *buf, uint16_t len);

/**
 * @brief  按资源ID将一段数据读入内部共享scratch缓冲区
 * @return 指向scratch缓冲区的指针(数据长度len), 失败返回0
 * @note   scratch为共享缓冲区(2048字节), 二次调用会覆盖上一次数据,
 *         调用方须在下次加载前完成使用
 */
const uint8_t *FlashStore_ResLoad(FlashStoreId id, uint32_t offset, uint16_t len);

/** @brief 获取共享scratch缓冲区指针(2048字节) */
uint8_t *FlashStore_Scratch(void);

/* ================================================================
 * 视频数据块烧录 (VIDEO_CHUNK_STORE_MODE)
 * 将嵌入固件的视频数据块(g_video_chunk)从外部Flash地址0(视频区域)偏移处
 * 顺序写入并回读CRC校验, 屏幕显示百分比进度。
 * 本函数不返回; 进入前需已初始化SPI Flash与LCD。
 * ================================================================ */
void VideoChunkStore_BurnAll(void);

#endif /* __FLASH_STORE_H__ */
