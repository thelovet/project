#ifndef __PLAYER_IMGS_H__
#define __PLAYER_IMGS_H__

#include <stdint.h>

#define PLAYER_COUNT 9

#define PLAYER_0_W  20
#define PLAYER_0_H  16
#define PLAYER_1_W  24
#define PLAYER_1_H  19
#define PLAYER_2_W  28
#define PLAYER_2_H  22
#define PLAYER_3_W  32
#define PLAYER_3_H  25
#define PLAYER_4_W  36
#define PLAYER_4_H  28
#define PLAYER_5_W  40
#define PLAYER_5_H  32
#define PLAYER_6_W  44
#define PLAYER_6_H  35
#define PLAYER_7_W  48
#define PLAYER_7_H  38
#define PLAYER_8_W  52
#define PLAYER_8_H  41

/* dims table: per player stage {w,h} */
extern const uint16_t player_imgs_dim[PLAYER_COUNT][2];

/* per-stage RGB565 little-endian byte stream, len = w*h*2 */
extern const uint8_t player_img_0[20*16*2];
extern const uint8_t player_img_1[24*19*2];
extern const uint8_t player_img_2[28*22*2];
extern const uint8_t player_img_3[32*25*2];
extern const uint8_t player_img_4[36*28*2];
extern const uint8_t player_img_5[40*32*2];
extern const uint8_t player_img_6[44*35*2];
extern const uint8_t player_img_7[48*38*2];
extern const uint8_t player_img_8[52*41*2];

#endif
