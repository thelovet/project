/**
 * @file    game_save.c
 * @brief   游戏状态存档模块: 将各游戏状态写入内部Flash存档页并在唤醒后恢复
 * @note    存档页固定 0x0800FF00(最后一个256字节页), 页头: 魔数/版本/模式/长度/CRC16;
 *          支持推箱子/2048/俄罗斯方块/华容道/大鱼吃小鱼(GameSave_IsSupportedMode)。
 *          唤醒恢复走 BKP 后备寄存器记恢复标志(视频/游戏), GameSave_RestoreWakeState
 *          读出后由 GameSave_RestorePending 调用各游戏 restore 接口。
 */

#include "string.h"
#include "ch32v20x_flash.h"
#include "game_save.h"
#include "sokoban.h"
#include "d2048.h"
#include "tetris.h"
#include "klotski.h"
#include "fish.h"

#define GAME_SAVE_PAGE_ADDR    0x0800FF00UL  /* 内部 Flash 最后一个 256 字节页的起始地址 */
#define GAME_SAVE_PAGE_SIZE    256           /* 存档页总大小，单位为字节 */
#define GAME_SAVE_HEADER_SIZE  12            /* 魔数、版本、模式、长度和 CRC 所占的页头大小 */
#define GAME_SAVE_PAYLOAD_SIZE (GAME_SAVE_PAGE_SIZE - GAME_SAVE_HEADER_SIZE) /* 游戏状态最大长度 */
#define GAME_SAVE_MAGIC        0x31565347UL  /* 存档有效标志，按小端字节序对应 "GSV1" */
#define GAME_SAVE_VERSION      2             /* 当前存档格式版本，用于拒绝旧格式数据 */
#define GAME_SAVE_BKP_MAGIC_VIDEO  0xA5A5    /* BKP 中的视频恢复类型标志 */
#define GAME_SAVE_BKP_MAGIC_GAME   0x5A5A    /* BKP 中的游戏恢复类型标志 */
#define GAME_SAVE_BKP_DR_MAGIC     BKP_DR1   /* 保存恢复类型标志的备份寄存器 */
#define GAME_SAVE_BKP_DR_STATE     BKP_DR2   /* 保存视频索引或游戏模式的备份寄存器 */

static uint32_t s_game_save_buffer[GAME_SAVE_PAGE_SIZE / sizeof(uint32_t)];
static uint8_t s_game_restore_pending = 0;
static MainMode s_game_restore_mode = MAIN_MODE_VIDEO;

/**
 * @brief  判断指定模式是否支持游戏存档
 * @param  mode  要判断的主模式
 * @return 1 表示支持存档，0 表示不支持
 */
uint8_t GameSave_IsSupportedMode(MainMode mode)
{
    return mode == MAIN_MODE_SOKOBAN || mode == MAIN_MODE_D2048
        || mode == MAIN_MODE_TETRIS || mode == MAIN_MODE_KLOTSKI
        || mode == MAIN_MODE_FISH;
}

/**
 * @brief  计算游戏存档载荷的 CRC16 校验值
 * @param  data  存档载荷数据
 * @param  len   存档载荷长度
 * @param  mode  游戏模式
 * @return 计算得到的 CRC16 校验值
 */
static uint16_t GameSave_Crc16(const uint8_t *data, uint8_t len, MainMode mode)
{
    uint16_t crc = (uint16_t)(0xFFFFU ^ ((uint16_t)mode << 8) ^ len);
    while (len--) {
        uint8_t bit;
        crc ^= (uint16_t)(*data++) << 8;
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1) ^ 0x1021U)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/**
 * @brief  调用对应游戏接口序列化当前状态
 * @param  mode  要保存的游戏模式
 * @param  buf   存档载荷输出缓冲区
 * @return 实际载荷长度，模式无效或序列化失败时返回 0
 */
static uint8_t GameSave_Serialize(MainMode mode, uint8_t *buf)
{
    switch (mode) {
        case MAIN_MODE_SOKOBAN:
            return sokoban_save_state(buf, GAME_SAVE_PAYLOAD_SIZE);
        case MAIN_MODE_D2048:
            return d2048_save_state(buf, GAME_SAVE_PAYLOAD_SIZE);
        case MAIN_MODE_TETRIS:
            return tetris_save_state(buf, GAME_SAVE_PAYLOAD_SIZE);
        case MAIN_MODE_KLOTSKI:
            return klotski_save_state(buf, GAME_SAVE_PAYLOAD_SIZE);
        case MAIN_MODE_FISH:
            return fish_save_state(buf, GAME_SAVE_PAYLOAD_SIZE);
        default:
            return 0;
    }
}

/**
 * @brief  将指定游戏状态写入内部 Flash 存档页
 * @param  mode  要保存的游戏模式
 * @return 1 表示写入或内容比对成功，0 表示保存失败
 */
uint8_t GameSave_Write(MainMode mode)
{
    uint8_t *page = (uint8_t *)s_game_save_buffer;
    const uint8_t *flash_page = (const uint8_t *)GAME_SAVE_PAGE_ADDR;
    uint32_t magic = GAME_SAVE_MAGIC;
    uint16_t crc;
    uint8_t len;

    memset(page, 0xFF, GAME_SAVE_PAGE_SIZE);
    len = GameSave_Serialize(mode, page + GAME_SAVE_HEADER_SIZE);
    if (len == 0) return 0;

    memcpy(page, &magic, sizeof(magic));
    page[4] = GAME_SAVE_VERSION;
    page[5] = (uint8_t)mode;
    page[6] = len;
    page[7] = (uint8_t)~len;
    crc = GameSave_Crc16(page + GAME_SAVE_HEADER_SIZE, len, mode);
    memcpy(page + 8, &crc, sizeof(crc));

    if (memcmp(flash_page, page, GAME_SAVE_PAGE_SIZE) == 0) return 1;

    __disable_irq();
    FLASH_Unlock_Fast();
    FLASH_ErasePage_Fast(GAME_SAVE_PAGE_ADDR);
    /*
     * Fast擦除偶有不彻底：抽查页中间位置，若未擦为0xFF则重试一次。
     * 第二次写入时Flash已有旧数据，依赖真擦除，首次空白页不受影响。
     */
    if (((const uint8_t *)GAME_SAVE_PAGE_ADDR)[GAME_SAVE_PAGE_SIZE / 2] != 0xFF) {
        FLASH_ErasePage_Fast(GAME_SAVE_PAGE_ADDR);
    }
    FLASH_ProgramPage_Fast(GAME_SAVE_PAGE_ADDR, s_game_save_buffer);
    FLASH_Lock_Fast();
    __enable_irq();
    return memcmp(flash_page, page, GAME_SAVE_PAGE_SIZE) == 0;
}

/**
 * @brief  校验内部 Flash 存档并恢复指定游戏
 * @param  mode  要恢复的游戏模式
 * @return 1 表示恢复成功，0 表示存档无效或恢复失败
 */
static uint8_t GameSave_Restore(MainMode mode)
{
    const uint8_t *page = (const uint8_t *)GAME_SAVE_PAGE_ADDR;
    uint32_t magic;
    uint16_t stored_crc;
    uint8_t len;

    memcpy(&magic, page, sizeof(magic));
    if (magic != GAME_SAVE_MAGIC || page[4] != GAME_SAVE_VERSION
        || page[5] != (uint8_t)mode) {
        return 0;
    }
    len = page[6];
    if (len == 0 || len > GAME_SAVE_PAYLOAD_SIZE
        || page[7] != (uint8_t)~len) {
        return 0;
    }
    memcpy(&stored_crc, page + 8, sizeof(stored_crc));
    if (stored_crc != GameSave_Crc16(page + GAME_SAVE_HEADER_SIZE, len, mode)) {
        return 0;
    }

    switch (mode) {
        case MAIN_MODE_SOKOBAN:
            return sokoban_restore_state(page + GAME_SAVE_HEADER_SIZE, len);
        case MAIN_MODE_D2048:
            return d2048_restore_state(page + GAME_SAVE_HEADER_SIZE, len);
        case MAIN_MODE_TETRIS:
            return tetris_restore_state(page + GAME_SAVE_HEADER_SIZE, len);
        case MAIN_MODE_KLOTSKI:
            return klotski_restore_state(page + GAME_SAVE_HEADER_SIZE, len);
        case MAIN_MODE_FISH:
            return fish_restore_state(page + GAME_SAVE_HEADER_SIZE, len);
        default:
            return 0;
    }
}

/**
 * @brief  将视频恢复标志和视频索引写入备份寄存器
 * @param  video_index  当前视频索引
 */
void GameSave_SaveVideoWakeState(uint8_t video_index)
{
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR | RCC_APB1Periph_BKP, ENABLE);
    PWR_BackupAccessCmd(ENABLE);
    BKP_WriteBackupRegister(GAME_SAVE_BKP_DR_MAGIC,
                            GAME_SAVE_BKP_MAGIC_VIDEO);
    BKP_WriteBackupRegister(GAME_SAVE_BKP_DR_STATE, video_index);
}

/**
 * @brief  将游戏恢复标志和游戏模式写入备份寄存器
 * @param  mode  休眠前的游戏模式
 */
void GameSave_SaveGameWakeState(MainMode mode)
{
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR | RCC_APB1Periph_BKP, ENABLE);
    PWR_BackupAccessCmd(ENABLE);
    BKP_WriteBackupRegister(GAME_SAVE_BKP_DR_MAGIC,
                            GAME_SAVE_BKP_MAGIC_GAME);
    BKP_WriteBackupRegister(GAME_SAVE_BKP_DR_STATE, (uint16_t)mode);
}

/**
 * @brief  读取并清除备份寄存器中的恢复状态
 * @param  video_index  视频索引输出地址
 * @param  video_count  视频总数
 * @return 视频、游戏或无恢复状态
 */
GameSaveWakeType GameSave_RestoreWakeState(uint8_t *video_index,
                                           uint8_t video_count)
{
    uint16_t magic;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_PWR | RCC_APB1Periph_BKP, ENABLE);
    PWR_BackupAccessCmd(ENABLE);
    magic = BKP_ReadBackupRegister(GAME_SAVE_BKP_DR_MAGIC);

    if (magic == GAME_SAVE_BKP_MAGIC_VIDEO) {
        *video_index =
            (uint8_t)BKP_ReadBackupRegister(GAME_SAVE_BKP_DR_STATE);
        if (*video_index >= video_count) *video_index = 0;
        BKP_WriteBackupRegister(GAME_SAVE_BKP_DR_MAGIC, 0x0000);
        return GAME_SAVE_WAKE_VIDEO;
    }

    if (magic == GAME_SAVE_BKP_MAGIC_GAME) {
        MainMode mode =
            (MainMode)BKP_ReadBackupRegister(GAME_SAVE_BKP_DR_STATE);
        BKP_WriteBackupRegister(GAME_SAVE_BKP_DR_MAGIC, 0x0000);
        if (GameSave_IsSupportedMode(mode)) {
            s_game_restore_mode = mode;
            s_game_restore_pending = 1;
            return GAME_SAVE_WAKE_GAME;
        }
    }

    return GAME_SAVE_WAKE_NONE;
}

/**
 * @brief  恢复备份寄存器指定的待恢复游戏
 * @param  mode  恢复成功或存档无效时输出目标游戏模式
 * @return 无待恢复状态、恢复成功或存档无效
 */
GameSavePendingResult GameSave_RestorePending(MainMode *mode)
{
    if (!s_game_restore_pending) return GAME_SAVE_PENDING_NONE;

    s_game_restore_pending = 0;
    *mode = s_game_restore_mode;
    s_game_restore_mode = MAIN_MODE_VIDEO;
    return GameSave_Restore(*mode) ? GAME_SAVE_PENDING_RESTORED
                                   : GAME_SAVE_PENDING_INVALID;
}
