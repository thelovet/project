#ifndef __FISH_IMGS_H__
#define __FISH_IMGS_H__

#include <stdint.h>

#define FISH_COUNT 10

#define FISH_0_W  16
#define FISH_0_H  12
#define FISH_1_W  20
#define FISH_1_H  16
#define FISH_2_W  24
#define FISH_2_H  16
#define FISH_3_W  28
#define FISH_3_H  19
#define FISH_4_W  32
#define FISH_4_H  19
#define FISH_5_W  36
#define FISH_5_H  21
#define FISH_6_W  40
#define FISH_6_H  28
#define FISH_7_W  44
#define FISH_7_H  22
#define FISH_8_W  48
#define FISH_8_H  28
#define FISH_9_W  52
#define FISH_9_H  31

/* dims table: per fish {w,h} */
extern const uint16_t fish_imgs_dim[FISH_COUNT][2];

/* per-fish RGB565 little-endian byte stream, len = w*h*2 */
extern const uint8_t fish_img_0[16*12*2];
extern const uint8_t fish_img_1[20*16*2];
extern const uint8_t fish_img_2[24*16*2];
extern const uint8_t fish_img_3[28*19*2];
extern const uint8_t fish_img_4[32*19*2];
extern const uint8_t fish_img_5[36*21*2];
extern const uint8_t fish_img_6[40*28*2];
extern const uint8_t fish_img_7[44*22*2];
extern const uint8_t fish_img_8[48*28*2];
extern const uint8_t fish_img_9[52*31*2];

#endif
