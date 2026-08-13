#ifndef __GAME_SAVE_H__
#define __GAME_SAVE_H__

#include "menu.h"

typedef enum
{
    GAME_SAVE_WAKE_NONE = 0,
    GAME_SAVE_WAKE_VIDEO = 1,
    GAME_SAVE_WAKE_GAME = 2
} GameSaveWakeType;

typedef enum
{
    GAME_SAVE_PENDING_NONE = 0,
    GAME_SAVE_PENDING_RESTORED = 1,
    GAME_SAVE_PENDING_INVALID = 2
} GameSavePendingResult;

/**
 * @brief  Check whether a mode supports game-state persistence.
 * @param  mode  Main mode to check.
 * @return 1 when supported, otherwise 0.
 */
uint8_t GameSave_IsSupportedMode(MainMode mode);

/**
 * @brief  Write a game state to the internal Flash save page.
 * @param  mode  Game mode to save.
 * @return 1 on success, otherwise 0.
 */
uint8_t GameSave_Write(MainMode mode);

/**
 * @brief  Save the video wake marker and video index to BKP.
 * @param  video_index  Current video index.
 */
void GameSave_SaveVideoWakeState(uint8_t video_index);

/**
 * @brief  Save the game wake marker and game mode to BKP.
 * @param  mode  Game mode before standby.
 */
void GameSave_SaveGameWakeState(MainMode mode);

/**
 * @brief  Read and clear the wake state stored in BKP.
 * @param  video_index  Output address for the video index.
 * @param  video_count  Number of available videos.
 * @return The stored wake-state type.
 */
GameSaveWakeType GameSave_RestoreWakeState(uint8_t *video_index,
                                           uint8_t video_count);

/**
 * @brief  Restore the pending game selected by the BKP state.
 * @param  mode  Output address for the pending game mode.
 * @return Pending restore result.
 */
GameSavePendingResult GameSave_RestorePending(MainMode *mode);

#endif
