/**
 * @file    fish.c
 * @brief   大鱼吃小鱼游戏模块
 *          触摸跟手控制大鱼移动, 小鱼随机游动;
 *          大鱼能吃掉比自己小的鱼并成长, 碰到比自己大的鱼则失败。
 *          鱼图片存于外部Flash, 运行时按大小DMA流式读取。
 */
#include "fish.h"
#include "fish_imgs.h"     /* 鱼图片尺寸/数据表 */
#include "player_imgs.h"   /* 玩家形象尺寸/数据表 */
#include "tetris.h"        /* 游戏返回码 */
#include "menu.h"          /* 菜单/退出/重开 */
#include "sok_img_win.h"   /* 16x32 数字字模 */
#include "flash_store.h"   /* 鱼图片资源从外部Flash读取 */
#include "lcd.h"
#include "display.h"
#include "touch.h"
#include "string.h"
#include "timer.h"
 /* 菜单: 退出菜单映射 + 行类型 (复用 menu.c 统一菜单系统) */
 /* 下划菜单游戏映射 (视频/推箱子/2048/华容道, 减当前=大鱼吃小鱼) */
static const uint8_t fish_exit_map[5] = {
    EXIT_RET_VIDEO, EXIT_RET_SOKOBAN, EXIT_RET_D2048, EXIT_RET_TETRIS, EXIT_RET_KLOTSKI
};
/* 退出菜单行类型 (供 menu_draw_nrow_frame_only / game_menu_drag_poll 使用) */
static const uint8_t fish_row_types[5] = {
    MENU_ROW_SHIPIN, MENU_ROW_TUI, MENU_ROW_2048, MENU_ROW_TETRIS, MENU_ROW_KLOTSKI
};

/* 游戏参数 */
#define FISH_PLAYER_START_LEVEL  0   /* 玩家物理初始等级0(第1条沙丁鱼); HUD显示为 等级+1 */
#define FISH_PLAYER_LEVEL_MAX    9   /* 玩家最大等级(物理9级=显示10级, 用最大形象PLAYER_COUNT-1) */
#define FISH_PLAYER_X   (LCD_W / 2)
#define FISH_FACE_DEAD    2    /* 转向死区(px): 手指静止时 X 抖动在此范围内不换朝向(防鱼原地左右翻) */
#define FISH_SMOOTH_DIV   3    /* 位置平滑强度: 每帧向目标靠近 1/该值(值大=更不抖但更滞后) */
#define FISH_MAX_LAG      8    /* 位置最大滞后(px): 与手指距离超过它就直接跟上, 否则快滑时鱼只走一半 */
#define FISH_PLAYER_Y   (LCD_H / 2)

#define FISH_NUM_FOOD   6      /* 场上小鱼数量 */
#define FISH_SPEED_BASE 2      /* 小鱼基础移动速度(降慢, 便于捕捉) */
#define FISH_MOVE_US    40000 /* 小鱼每40ms移动一次(基于真实时间, 更连续) */
#define FISH_LIFE_TICKS     150   /* 可吃小鱼(<=玩家级)最长停留圈数, 40ms/圈≈6s */
#define FISH_LIFE_BIG_TICKS  60    /* 比玩家大的鱼更快离开(≈2.4s), 避免场上全是吃不到的大鱼 */
#define FISH_LEAVE_SPEED     2      /* 游出屏幕速度(px/40ms), 与普通小鱼一致保持自然移动 */
#define FISH_FOOD_RANGE 3      /* 自适应难度: 小鱼等级在玩家左右 ±3 内 */
#define FISH_INVINCIBLE 0      /* 无敌模式(1=无敌: 碰到更大鱼忽略不撞死也不吃; 0=正常玩法, 碰大鱼即失败) */
#define FISH_WIN_SCORE     1953125   /* 通关目标分数: 达到1,953,125分(显示10级)即通关 */

/* 存档缓冲布局宏 */
#define FISH_SAVE_BASE 11   /* 玩家 level+x+y+facing(6) + score(4) + state(1) */
#define FISH_SAVE_FOOD 9    /* 每条鱼: active+level+x+y+vx+vy */
#define FISH_SAVE_LEN (FISH_SAVE_BASE + FISH_NUM_FOOD * FISH_SAVE_FOOD)

/* 玩家形象/尺寸级别(封顶到最大图片) */
#define PLAYER_IMG_LVL ((g_player.level < PLAYER_COUNT) ? g_player.level : (PLAYER_COUNT - 1))

/* 顶部得分条区域(16x32数字从y=4到y=35), 实体不得越过, 避免游动时擦掉计分 */
#define SCORE_TOP      4
#define SCORE_H        32
/* 顶部信息栏数字大小为8x14, 信息栏占 y4~31; PLAY_TOP=32 让鱼顶贴信息栏下缘 */
#define PLAY_TOP       32   /* 游戏区顶部边界(信息栏底31, 留1px) */
#define PLAY_Y_MIN    (PLAY_TOP + 20)           /* 最高实体半高(player=20), 防顶图越界 */
#define PLAY_Y_MAX    (LCD_H - 21)              /* 高度最大41(半高20)时底边仍完整 */
#define PLAY_X_MIN    26                        /* 宽度最大52(半宽26)时左右仍完整 */
#define PLAY_X_MAX    (LCD_W - 26)
#define PLAY_CORNER   30   /* 8角形倒角边长: 切掉矩形四角, 让鱼不进入圆形屏被遮挡的四角 */

/* 游戏状态 */
typedef struct {
    uint8_t active;      /* 是否在场 */
    uint8_t level;       /* 等级 0-9 */
    uint8_t dir;         /* 游动方向(0-7) */
    uint16_t x, y;       /* 当前位置 */
    int16_t vx, vy;      /* 速度分量 */
    int16_t prev_x, prev_y;  /* 上一帧位置(增量擦除用) */
    uint8_t prev_level;  /* 上一帧等级(尺寸变化后擦除旧图用) */
    uint16_t life;
    uint8_t leaving;     /* 走出屏幕中标志 */
    uint8_t entering;    /* 从边缘入场中标志 */
    uint8_t facing;      /* 朝向: 0=右, 1=左(仅玩家使用) */
    uint8_t drawn;       /* 该实体已绘制过(用于只画变化) */
} FishEntity;

static uint8_t   g_state = FISH_STATE_PLAYING;
static uint8_t   g_won;  /* 胜利标志: 1=达到1,953,125分通关 */
static FishEntity g_player;          /* 玩家大鱼 */
static FishEntity g_food[FISH_NUM_FOOD];  /* 小鱼 */
static uint32_t  g_score;            /* 累积分(吃鱼得分 = 5^等级: 0级=1,1级=5,2级=25...) */
static uint8_t   g_hud_dirty;        /* 顶部信息(分数/进度)需重绘 */
static uint16_t  g_frame;            /* 帧计数器(用于动画/移动节拍) */
static uint32_t  g_food_us;          /* 上次小鱼移动的时间戳(微秒, 真实时间节拍) */
static uint8_t   g_full_redraw;      /* 全屏重绘脏标记 */
static int16_t   s_drag_start_fx;    /* 拖动起点: 鱼的X (绝对位置法, 与菜单拖动同理) */
static int16_t   s_drag_start_fy;    /* 拖动起点: 鱼的Y */
static int16_t   s_drag_start_tx;    /* 拖动起点: 手指X */
static int16_t   s_drag_start_ty;    /* 拖动起点: 手指Y */
/* 拖动只需"绝对位置法"的起点记录(见 s_drag_start_*), 位置不另做滤波以保跟手 */
static int16_t   s_prev_px;          /* 上一帧鱼X(用于累积位移判断转向) */
static int16_t   s_face_accum;       /* 转向累积: 平滑后位置的单向位移累计, 够了才换朝向 */

/* 颜色定义 (RGB565) */
#define COLOR_BG     0x14A6   /* 深海蓝背景 */
#define COL_RED      0xF800   /* 结束提示-红 */
/* 判断像素是否视为"透明": 接近白色(含纯白)也不显示, 避免边缘残留白边 */
#define IS_TRANSPARENT(p)  (((((p) >> 11) & 0x1F) >= 0x1C) && \
                            (((((p) >> 5) & 0x3F)) >= 0x38) && \
                            (((p) & 0x1F) >= 0x1C))

/**
 * @brief  内部工具: 随机数 (复用 game_rand)
 * @param  n 上限
 * @return [0, n) 的随机数
 */
static uint16_t fish_rand(uint16_t n) {
    if (n == 0) return 0;
    return (uint16_t)(game_rand() % n);
}

/**
 * @brief  将鱼/玩家中心点钳制到"8角形"可游动区(矩形四角倒角, 贴合圆形屏)。
 *         倒角后中心点不再进入圆形屏被遮挡/裁切的四角。
 * @param  px,py 指针: 入=当前位置, 出=钳制后位置
 */
static void fish_clamp_pos(int16_t* px, int16_t* py) {
    int16_t x = *px, y = *py;
    /* 矩形钳位 */
    if (x < PLAY_X_MIN) x = PLAY_X_MIN;
    if (x > PLAY_X_MAX) x = PLAY_X_MAX;
    if (y < PLAY_Y_MIN) y = PLAY_Y_MIN;
    if (y > PLAY_Y_MAX) y = PLAY_Y_MAX;
    /* 四角倒角: 角部(离两邻边都 < PLAY_CORNER)时, 把中心点推回45°斜边内 */
    if (x < PLAY_X_MIN + PLAY_CORNER && y < PLAY_Y_MIN + PLAY_CORNER) {
        if ((x - PLAY_X_MIN) + (y - PLAY_Y_MIN) < PLAY_CORNER)
            x = PLAY_X_MIN + (int16_t)(PLAY_CORNER - (y - PLAY_Y_MIN));
    }
    if (x > PLAY_X_MAX - PLAY_CORNER && y < PLAY_Y_MIN + PLAY_CORNER) {
        if ((PLAY_X_MAX - x) + (y - PLAY_Y_MIN) < PLAY_CORNER)
            x = PLAY_X_MAX - (int16_t)(PLAY_CORNER - (y - PLAY_Y_MIN));
    }
    if (x < PLAY_X_MIN + PLAY_CORNER && y > PLAY_Y_MAX - PLAY_CORNER) {
        if ((x - PLAY_X_MIN) + (PLAY_Y_MAX - y) < PLAY_CORNER)
            x = PLAY_X_MIN + (int16_t)(PLAY_CORNER - (PLAY_Y_MAX - y));
    }
    if (x > PLAY_X_MAX - PLAY_CORNER && y > PLAY_Y_MAX - PLAY_CORNER) {
        if ((PLAY_X_MAX - x) + (PLAY_Y_MAX - y) < PLAY_CORNER)
            x = PLAY_X_MAX - (int16_t)(PLAY_CORNER - (PLAY_Y_MAX - y));
    }
    *px = x;
    *py = y;
}

/**
 * @brief  小鱼角落反弹: 在8角形的斜边处, 若速度仍朝外, 把速度反转向内
 *         并贴回斜边, 避免鱼卡在角落要等随机变向才动。
 * @param  f 小鱼实体
 */
static void fish_bounce_corners(FishEntity* f) {
    int16_t X = (int16_t)f->x, Y = (int16_t)f->y;
    int16_t dxl = X - PLAY_X_MIN, dxr = PLAY_X_MAX - X;
    int16_t dyt = Y - PLAY_Y_MIN, dyb = PLAY_Y_MAX - Y;
    if (dxl < PLAY_CORNER && dyt < PLAY_CORNER && f->vx <= 0 && f->vy <= 0) {
        f->vx = (int16_t)-f->vx; f->vy = (int16_t)-f->vy;
        X = PLAY_X_MIN + (int16_t)(PLAY_CORNER - dyt);
    }
    else if (dxr < PLAY_CORNER && dyt < PLAY_CORNER && f->vx >= 0 && f->vy <= 0) {
        f->vx = (int16_t)-f->vx; f->vy = (int16_t)-f->vy;
        X = PLAY_X_MAX - (int16_t)(PLAY_CORNER - dyt);
    }
    else if (dxl < PLAY_CORNER && dyb < PLAY_CORNER && f->vx <= 0 && f->vy >= 0) {
        f->vx = (int16_t)-f->vx; f->vy = (int16_t)-f->vy;
        X = PLAY_X_MIN + (int16_t)(PLAY_CORNER - dyb);
    }
    else if (dxr < PLAY_CORNER && dyb < PLAY_CORNER && f->vx >= 0 && f->vy >= 0) {
        f->vx = (int16_t)-f->vx; f->vy = (int16_t)-f->vy;
        X = PLAY_X_MAX - (int16_t)(PLAY_CORNER - dyb);
    }
    f->x = (uint16_t)X;
    f->y = (uint16_t)Y;
}

/**
 * @brief  升级/得分阈值: 返回 5^lv (0级=1, 1级=5, 2级=25...)
 * @param  lv 等级(<=9 安全, uint32)
 * @return 5^lv
 */
static uint32_t fish_thresh(uint8_t lv) {
    uint32_t v = 1;
    uint8_t i;
    for (i = 0; i < lv; i++) v *= 5;
    return v;
}

/**
 * @brief  把 uint32 分数转成各位数字。
 * @param  v      分数
 * @param  digits 输出缓冲(digits[0]=个位 ... digits[n-1]=最高位)
 * @return 产生的位数
 */
static uint8_t fish_score_digits(uint32_t v, uint8_t* digits) {
    uint8_t n = 0;
    do {
        if (n >= 8) break;
        digits[n++] = (uint8_t)(v % 10);
        v /= 10;
    } while (v > 0);
    return n;
}

/* 前向声明: clear_entity_rect 内部需重绘相交实体 */
static void draw_image_sprite(FlashStoreId id_base, const uint16_t dims[][2],
    uint8_t count, uint8_t level, uint16_t x, uint16_t y,
    uint8_t flip);

/**
 * @brief  擦除一个实体的矩形区域, 并立即重绘所有与该区域相交的其他实体。
 * @param  dims  尺寸表
 * @param  level 等级
 * @param  cx   中心 X
 * @param  cy   中心 Y
 * @note   透明图增量绘制用背景色填满某实体旧矩形时, 会误伤与其重叠的其他实体像素;
 *         因此填背景后立即按固定顺序重绘矩形内的所有实体, 防止残留。
 */
/**
 * @brief  重绘与给定矩形相交的实体(玩家/小鱼), 不动背景。
 * @note   供"只用背景色擦掉低电图标线条"的恢复路径复用(见 fish_erase_lowbatt_icon):
 *         那条路不能整块填背景 —— 会把框内的鱼/玩家一起擦掉再重画, 而重画要从外部
 *         Flash 流式读精灵图, 那期间框内是空的, 用户看到"电池框内部黑一下"。
 */
static void redraw_entities_in_rect(const uint16_t dims[][2], uint8_t level,
    int16_t cx, int16_t cy) {
    uint16_t w = dims[level][0];
    uint16_t h = dims[level][1];
    int16_t x0 = cx - (int16_t)(w / 2);
    int16_t y0 = cy - (int16_t)(h / 2);
    int16_t x1 = x0 + (int16_t)w - 1;
    int16_t y1 = y0 + (int16_t)h - 1;
    if (x0 < 0) x0 = 0;
    if (y0 < PLAY_TOP) y0 = PLAY_TOP;
    if (x1 > 239) x1 = 239;
    if (y1 > 239) y1 = 239;
    if (x1 < x0 || y1 < y0) return;

    uint16_t pw = player_imgs_dim[PLAYER_IMG_LVL][0];
    uint16_t ph = player_imgs_dim[PLAYER_IMG_LVL][1];
    int16_t px0 = (int16_t)g_player.x - (int16_t)(pw / 2);
    int16_t py0 = (int16_t)g_player.y - (int16_t)(ph / 2);
    int16_t px1 = px0 + (int16_t)pw - 1;
    int16_t py1 = py0 + (int16_t)ph - 1;
    if (px1 >= x0 && px0 <= x1 && py1 >= y0 && py0 <= y1)
        draw_image_sprite(FLASH_STORE_ID_PLAYER_0, player_imgs_dim,
            PLAYER_COUNT, g_player.level, g_player.x, g_player.y, g_player.facing);
    uint8_t i;
    for (i = 0; i < FISH_NUM_FOOD; i++) {
        FishEntity* f = &g_food[i];
        if (!f->active) continue;
        uint16_t fw = fish_imgs_dim[f->level][0];
        uint16_t fh = fish_imgs_dim[f->level][1];
        int16_t fx0 = (int16_t)f->x - (int16_t)(fw / 2);
        int16_t fy0 = (int16_t)f->y - (int16_t)(fh / 2);
        int16_t fx1 = fx0 + (int16_t)fw - 1;
        int16_t fy1 = fy0 + (int16_t)fh - 1;
        if (fx1 >= x0 && fx0 <= x1 && fy1 >= y0 && fy0 <= y1)
            draw_image_sprite(FLASH_STORE_ID_FISH_0, fish_imgs_dim,
                FISH_COUNT, f->level, f->x, f->y, f->facing);
    }
}

/**
 * @brief  用背景色擦掉低电大图标(只擦线条/凸点/感叹号那几条窄带)
 * @note   几何与 display.c 的 LowBattery_ComputeGeo 一致(bx=45,by=79,bw=150,bh=82)。
 *         不整块填背景: 框内本来就是游戏画面, 没有任何东西需要恢复; 只把图标画上去
 *         的那几条线擦回背景色即可 —— 线只有 2px 宽, 几乎碰不到框内的鱼/玩家。
 */
static void fish_erase_lowbatt_icon(void) {
    LCD_Fill_DMA(45,  79, 195,  81, COLOR_BG);   /* 上边(2px) */
    LCD_Fill_DMA(45, 159, 195, 161, COLOR_BG);   /* 下边(2px) */
    LCD_Fill_DMA(45,  79,  47, 161, COLOR_BG);   /* 左边(2px) */
    LCD_Fill_DMA(193, 79, 195, 161, COLOR_BG);   /* 右边(2px) */
    LCD_Fill_DMA(195, 104, 210, 136, COLOR_BG);  /* 右凸点 */
    LCD_Fill_DMA(111, 92, 129, 128, COLOR_BG);   /* 感叹号竖条 */
    LCD_Fill_DMA(111, 137, 129, 155, COLOR_BG);  /* 感叹号点 */
}

static void clear_entity_rect(const uint16_t dims[][2], uint8_t level,
    int16_t cx, int16_t cy) {
    uint16_t w = dims[level][0];
    uint16_t h = dims[level][1];
    int16_t x0 = cx - (int16_t)(w / 2);
    int16_t y0 = cy - (int16_t)(h / 2);
    int16_t x1 = x0 + (int16_t)w - 1;
    int16_t y1 = y0 + (int16_t)h - 1;
    if (x0 < 0) x0 = 0;
    if (y0 < PLAY_TOP) y0 = PLAY_TOP;
    if (x1 > 239) x1 = 239;
    if (y1 > 239) y1 = 239;
    if (x1 < x0 || y1 < y0) return;
    /* 填背景(LCD_Fill 的 end 不含坐标); 走 DMA: 低电灭段恢复时这块是 166x83≈13.8K 像素,
       CPU 逐像素写要好几毫秒, 那几毫秒里屏幕上还是刚被"刷黑"的图标线条 → 看着黑一下 */
    LCD_Fill_DMA((uint16_t)x0, (uint16_t)y0, (uint16_t)(x1 + 1), (uint16_t)(y1 + 1), COLOR_BG);
    /* 重绘所有与该矩形相交的实体(按当前坐标), 避免被背景误伤 */
    redraw_entities_in_rect(dims, level, cx, cy);
}

/**
 * @brief  绘制一张精灵图(按等级从外部 Flash 流式读取对应图, 跳过白色背景)。
 * @param  id_base 资源起始 ID(FLASH_STORE_ID_*_0)
 * @param  dims    尺寸表(count×2, 每等级 {w,h})
 * @param  count   图张数
 * @param  level   第几张(0~count-1, 越界钳位)
 * @param  x       中心 X
 * @param  y       中心 Y
 * @param  flip    1=水平镜像
 */
static void draw_image_sprite(FlashStoreId id_base, const uint16_t dims[][2],
    uint8_t count, uint8_t level,
    uint16_t x, uint16_t y, uint8_t flip) {
    if (level >= count) {
        if (count == 0) return;
        level = (uint8_t)(count - 1);   /* 超过最大等级: 用最大一张 */
    }
    uint16_t w = dims[level][0];
    uint16_t h = dims[level][1];
    FlashStoreId id = (FlashStoreId)(id_base + level);
    const uint8_t* img = FlashStore_ResLoad(id, 0, (uint16_t)(w * h * 2));
    if (!img) return;

    const uint16_t* px = (const uint16_t*)img;   /* 小端序强转 */
    /* 左上角(中心对齐) */
    int16_t x0 = (int16_t)(x)-(int16_t)(w / 2);
    int16_t y0 = (int16_t)(y)-(int16_t)(h / 2);

    uint16_t r, c;
    LCD_CS_CLR;
    for (r = 0; r < h; r++) {
        int16_t sy = y0 + (int16_t)r;
        if (sy < 0 || sy >= 240) continue;
        c = 0;
        while (c < w) {
            /* 跳过接近白色背景(透明) */
            while (c < w && IS_TRANSPARENT(px[r * w + c])) c++;
            if (c >= w) break;
            uint16_t start = c;
            /* 收集连续非透明(非白)段 */
            while (c < w && !IS_TRANSPARENT(px[r * w + c])) c++;
            /* 段 [start, c), 屏幕X裁剪; flip=1 时水平镜像 */
            int16_t sx0, sx1;
            if (flip) {
                sx0 = x0 + (int16_t)(w - c);
                sx1 = x0 + (int16_t)(w - 1 - start);
            }
            else {
                sx0 = x0 + (int16_t)start;
                sx1 = x0 + (int16_t)c - 1;
            }
            if (sx1 < 0 || sx0 >= 240) continue;
            if (sx0 < 0) sx0 = 0;
            if (sx1 > 239) sx1 = 239;
            if (sx1 < sx0) continue;
            LCD_Address_Set((uint16_t)sx0, (uint16_t)sy,
                (uint16_t)sx1, (uint16_t)sy);
            for (uint16_t i = 0; i < (uint16_t)(c - start); i++) {
                if (flip) LCD_WR_DATA_16Bit(px[r * w + (c - 1 - i)]);
                else      LCD_WR_DATA_16Bit(px[r * w + start + i]);
            }
        }
    }
    LCD_CS_SET;
}

/**
 * @brief  小鱼移动的行级原子重绘: 对"旧∪新"区域逐行 "先写该行背景、再写该行新鱼像素",
 *         避免整块背景填充造成的瞬间闪烁; 之后重绘区域内其它相交实体(防误伤)。
 * @param  f 小鱼实体
 */
static void fish_move_redraw(const FishEntity* f) {
    uint16_t w = fish_imgs_dim[f->level][0];
    uint16_t h = fish_imgs_dim[f->level][1];
    uint16_t ow = fish_imgs_dim[f->prev_level][0];
    uint16_t oh = fish_imgs_dim[f->prev_level][1];
    int16_t nx0 = (int16_t)f->x - (int16_t)(w / 2);
    int16_t ny0 = (int16_t)f->y - (int16_t)(h / 2);
    int16_t nx1 = nx0 + (int16_t)w - 1;
    int16_t ny1 = ny0 + (int16_t)h - 1;
    int16_t ox0 = (int16_t)f->prev_x - (int16_t)(ow / 2);
    int16_t oy0 = (int16_t)f->prev_y - (int16_t)(oh / 2);
    int16_t ox1 = ox0 + (int16_t)ow - 1;
    int16_t oy1 = oy0 + (int16_t)oh - 1;
    int16_t rx0 = (nx0 < ox0) ? nx0 : ox0;
    int16_t ry0 = (ny0 < oy0) ? ny0 : oy0;
    int16_t rx1 = (nx1 > ox1) ? nx1 : ox1;
    int16_t ry1 = (ny1 > oy1) ? ny1 : oy1;
    if (rx0 < 0) rx0 = 0;
    if (ry0 < PLAY_TOP) ry0 = PLAY_TOP;
    if (rx1 > 239) rx1 = 239;
    if (ry1 > 239) ry1 = 239;
    if (rx1 < rx0 || ry1 < ry0) return;

    const uint8_t* img = FlashStore_ResLoad((FlashStoreId)(FLASH_STORE_ID_FISH_0 + f->level),
        0, (uint16_t)(w * h * 2));
    if (!img) return;
    const uint16_t* px = (const uint16_t*)img;

    LCD_CS_CLR;
    for (int16_t y = ry0; y <= ry1; y++) {
        /* 该行先写背景(不跨越行, 撕裂窗口收缩到单行, 减闪) */
        LCD_Address_Set((uint16_t)rx0, (uint16_t)y, (uint16_t)rx1, (uint16_t)y);
        for (int16_t x = rx0; x <= rx1; x++) LCD_WR_DATA_16Bit(COLOR_BG);
        /* 该行新鱼的非白段(覆盖背景) */
        int16_t fy = y - ny0;
        if (fy >= 0 && fy < (int16_t)h) {
            int16_t c = 0;
            while (c < (int16_t)w) {
                while (c < (int16_t)w && IS_TRANSPARENT(px[fy * w + c])) c++;
                if (c >= (int16_t)w) break;
                int16_t start = c;
                while (c < (int16_t)w && !IS_TRANSPARENT(px[fy * w + c])) c++;
                int16_t sx0, sx1;
                if (f->facing) {
                    sx0 = nx0 + (int16_t)(w - c);
                    sx1 = nx0 + (int16_t)(w - 1 - start);
                }
                else {
                    sx0 = nx0 + start;
                    sx1 = nx0 + (int16_t)c - 1;
                }
                if (sx1 < rx0 || sx0 > rx1) continue;
                if (sx0 < rx0) sx0 = rx0;
                if (sx1 > rx1) sx1 = rx1;
                if (sx1 < sx0) continue;
                LCD_Address_Set((uint16_t)sx0, (uint16_t)y, (uint16_t)sx1, (uint16_t)y);
                for (uint16_t i = (uint16_t)(sx0 - nx0); i <= (uint16_t)(sx1 - nx0); i++)
                    if (f->facing) LCD_WR_DATA_16Bit(px[fy * w + (w - 1 - i)]);
                    else           LCD_WR_DATA_16Bit(px[fy * w + i]);
            }
        }
    }
    LCD_CS_SET;

    /* 重绘与区域相交的其它实体(当前坐标), 避免行背景误伤它们 */
    {
        uint16_t pw = player_imgs_dim[PLAYER_IMG_LVL][0];
        uint16_t ph = player_imgs_dim[PLAYER_IMG_LVL][1];
        int16_t px0 = (int16_t)g_player.x - (int16_t)(pw / 2);
        int16_t py0 = (int16_t)g_player.y - (int16_t)(ph / 2);
        int16_t px1 = px0 + (int16_t)pw - 1;
        int16_t py1 = py0 + (int16_t)ph - 1;
        if (px1 >= rx0 && px0 <= rx1 && py1 >= ry0 && py0 <= ry1)
            draw_image_sprite(FLASH_STORE_ID_PLAYER_0, player_imgs_dim,
                PLAYER_COUNT, g_player.level, g_player.x, g_player.y, g_player.facing);
        uint8_t i;
        for (i = 0; i < FISH_NUM_FOOD; i++) {
            const FishEntity* of = &g_food[i];
            if (of == f || !of->active) continue;
            uint16_t fw = fish_imgs_dim[of->level][0];
            uint16_t fh = fish_imgs_dim[of->level][1];
            int16_t fx0 = (int16_t)of->x - (int16_t)(fw / 2);
            int16_t fy0 = (int16_t)of->y - (int16_t)(fh / 2);
            int16_t fx1 = fx0 + (int16_t)fw - 1;
            int16_t fy1 = fy0 + (int16_t)fh - 1;
            if (fx1 >= rx0 && fx0 <= rx1 && fy1 >= ry0 && fy0 <= ry1)
                draw_image_sprite(FLASH_STORE_ID_FISH_0, fish_imgs_dim,
                    FISH_COUNT, of->level, of->x, of->y, of->facing);
        }
    }
}

/**
 * @brief  清屏背景
 */
static void draw_background(void) {
    LCD_Fill(0, 0, LCD_W, LCD_H, COLOR_BG);
}

/**
 * @brief  绘制顶部信息栏: 左=当前分数, 右=升级进度条(10格, 按分数在当前级的占比)
 */
/**
 * @brief  顶部信息栏第1行: 当前分数(居中, 黄色 8x14)
 * @note   拆成小函数是为了让分支距离变短: 合成一个大函数时 GCC 会报
 *         "R_RISCV_BRANCH truncated to fit", 链接失败(见备忘录的同类坑)
 */
static void draw_hud_score(void) {
    uint8_t digits[8];
    uint8_t dc = fish_score_digits(g_score, digits);   /* 低位在前 */
    uint16_t score_w = (uint16_t)dc * 8;
    uint16_t sx = (uint16_t)((240 - score_w) / 2);
    uint8_t i;
    for (i = 0; i < dc; i++) {
        uint8_t d = digits[dc - 1 - i];
        sok_draw_digit_scaled((uint16_t)(sx + i * 8), 4, d, LCD_YELLOW, 8, 14);
    }
}

/**
 * @brief  顶部信息栏第2行: 升级进度条(10格) + 当前等级数字(紧贴进度条右侧)
 */
static void draw_hud_progress(void) {
    uint16_t prog_w = 10 * 4 + 9 * 1;         /* 进度条总宽49 */
    uint16_t pl_total = prog_w + 2 + 16;      /* 进度+间隔+等级(最多2位) */
    uint16_t px = (uint16_t)((240 - pl_total) / 2);
    uint16_t py = 18;
    uint8_t j;
    uint32_t lo = (g_player.level > 0) ? fish_thresh(g_player.level) : 0;
    uint32_t hi = fish_thresh((uint8_t)(g_player.level + 1));
    uint32_t seg = hi - lo;
    uint8_t fill = 0;
    if (seg > 0) {
        uint32_t cur = (g_score >= lo) ? (g_score - lo) : 0;
        fill = (uint8_t)((cur * 10) / seg);
        if (fill > 10) fill = 10;
    }
    for (j = 0; j < 10; j++) {
        uint16_t col = (j < fill) ? 0xFFE0 : 0x1E69;
        LCD_Fill(px, py, (uint16_t)(px + 4), (uint16_t)(py + 13), col);
        px += 4 + 1;
    }
    px += 2;
    LCD_CS_CLR;   /* 进度条 LCD_Fill 内部会切 CS, 画等级前须重新拉低片选, 否则写不进 */
    {
        uint8_t lv = (uint8_t)(g_player.level + 1);
        if (lv >= 10) {
            sok_draw_digit_scaled(px, py, 1, LCD_WHITE, 8, 14);
            sok_draw_digit_scaled((uint16_t)(px + 8), py, (uint8_t)(lv - 10), LCD_WHITE, 8, 14);
        }
        else {
            sok_draw_digit_scaled(px, py, lv, LCD_WHITE, 8, 14);
        }
    }
}

/**
 * @brief  绘制顶部信息栏: 第1行分数, 第2行升级进度条+等级
 */
static void draw_hud(void) {
    if (!g_hud_dirty) return;
    g_hud_dirty = 0;
    /* 清空顶部信息区 */
    LCD_Fill(0, SCORE_TOP, LCD_W, PLAY_TOP, COLOR_BG);
    {
        LCD_CS_CLR;
        draw_hud_score();
        draw_hud_progress();
        LCD_CS_SET;
    }
}

/**
 * @brief  绘制游戏结束画面的"结束文字+分数"(不含背景).
 * @note   供低电图标灭段在图标区背景上复用重绘, 避免整屏清黑闪一下.
 */
static void draw_game_over_text(void) {
    uint16_t y = (240 - 32) / 2 - 14;
    if (g_won) {
        /* "通关" / "Cleared" (绿色, 居中) */
        sok_draw_big_text(SOK_BIGTEXT_CLEARED, y, LCD_GREEN);
    }
    else {
        /* "游戏结束" / "Game Over" (红色, 居中) */
        sok_draw_big_text(SOK_BIGTEXT_GAMEOVER, y, LCD_RED);
    }
    /* 通关/结束文字下方显示当前分数 (黄色 16x26, 居中) */
    {
        uint8_t digits[8];
        uint8_t dc = fish_score_digits(g_score, digits);  /* 低位在前 */
        uint16_t tw = 16, th = 26;
        uint16_t sx = (uint16_t)((240 - (uint16_t)dc * tw) / 2);
        uint16_t sy = 150;
        uint8_t i;
        LCD_CS_CLR;   /* 前面 LCD_Fill/写字已切 CS, 画数字前须重新拉低片选 */
        for (i = 0; i < dc; i++)
            sok_draw_digit_scaled(sx + i * tw, sy, digits[dc - 1 - i], LCD_YELLOW, tw, th);
        LCD_CS_SET;
    }
}

/**
 * @brief  绘制游戏结束画面: 背景 + "游戏结束"文字 + 分数 + 顶部HUD
 */
static void draw_game_over(void) {
    LCD_Fill(0, 0, LCD_W, LCD_H, COLOR_BG);
    draw_game_over_text();
    g_hud_dirty = 1;
    draw_hud();
}

/**
 * @brief  初始化一条小鱼 (随机位置/等级/方向)
 * @param  e             小鱼实体
 * @param  avoid_player 是否避开玩家(1=考虑与玩家距离)
 */
static void food_respawn(FishEntity* e, uint8_t avoid_player) {
    uint8_t was_drawn = e->drawn;
    e->active = 1;
    /* 大多小鱼 <=玩家级别(可吃, 供成长), 少数更大(挑战); 避免开局无鱼可吃 */
    if (fish_rand(4) != 0) {
        /* 75%: 等级 0..玩家级别 (含同级, 可吃) */
        e->level = (uint8_t)fish_rand((uint16_t)(g_player.level + 1));
    }
    else {
        /* 25%: 玩家级别+1 .. +2 (更大, 碰撞失败) */
        int16_t lo = (int16_t)g_player.level + 1;
        int16_t hi = (int16_t)g_player.level + 2;
        if (hi > 9) hi = 9;
        if (lo > 9) lo = 9;
        if (hi < lo) hi = lo;
        e->level = (uint8_t)(lo + (int16_t)fish_rand((uint16_t)(hi - lo + 1)));
    }
    if (e->level > 9) e->level = 9;
    /* 从随机屏幕边缘外入场(下/左/右), 若与已有鱼出现位置重叠则沿边偏移错开 */
    {
        uint8_t edge = (uint8_t)fish_rand(3);
        if (edge == 0) {          /* 下边(往上游入) */
            e->x = (uint16_t)(60 + fish_rand(120));
            e->y = (uint16_t)(LCD_H + 20);
            e->vx = 0; e->vy = -(int16_t)FISH_SPEED_BASE;
        }
        else if (edge == 1) {   /* 左边(往右游入) */
            e->x = (uint16_t)(-20);
            e->y = (uint16_t)(85 + fish_rand(100));
            e->vx = (int16_t)FISH_SPEED_BASE; e->vy = 0;
        }
        else {                  /* 右边(往左游入) */
            e->x = (uint16_t)(LCD_W + 20);
            e->y = (uint16_t)(85 + fish_rand(100));
            e->vx = -(int16_t)FISH_SPEED_BASE; e->vy = 0;
        }
        /* 出现位置与其他鱼重叠时, 沿边平移错开 */
        {
            uint8_t tries;
            for (tries = 0; tries < 8; tries++) {
                int16_t overlap = 0;
                uint8_t i2;
                for (i2 = 0; i2 < FISH_NUM_FOOD; i2++) {
                    FishEntity* o = &g_food[i2];
                    if (o == e || !o->active) continue;
                    uint16_t ow = fish_imgs_dim[o->level][0];
                    uint16_t oh = fish_imgs_dim[o->level][1];
                    uint16_t ew = fish_imgs_dim[e->level][0];
                    uint16_t eh = fish_imgs_dim[e->level][1];
                    int16_t adx = (int16_t)o->x - (int16_t)e->x;
                    int16_t ady = (int16_t)o->y - (int16_t)e->y;
                    if (adx < 0) adx = (int16_t)-adx;
                    if (ady < 0) ady = (int16_t)-ady;
                    if (adx < (int16_t)((ow + ew) / 2) &&
                        ady < (int16_t)((oh + eh) / 2)) {
                        overlap = 1; break;
                    }
                }
                if (!overlap) break;
                if (edge == 0) e->x = (uint16_t)((int16_t)e->x + 12);
                else           e->y = (uint16_t)((int16_t)e->y + 12);
            }
        }
        e->entering = 1;
    }
    e->dir = (uint8_t)fish_rand(8);
    e->facing = (e->vx < 0) ? 1 : 0;   /* 按入场方向定朝向 */
    /* 先换好新位置再擦旧图: clear 用 prev(旧) 算旧矩形, 重绘相交实体用当前(已更新)坐标, 不会把旧位置又画回 */
    if (was_drawn)
        clear_entity_rect(fish_imgs_dim, e->prev_level, e->prev_x, e->prev_y);
    e->prev_x = (int16_t)e->x;
    e->prev_y = (int16_t)e->y;
    e->prev_level = e->level;
    e->drawn = 0;
    e->life = 0;
    e->leaving = 0;
    (void)avoid_player;
}

/**
 * @brief  绘制鱼游戏全套场景(背景+玩家+所有小鱼+HUD)。
 * @note   用于菜单切换后恢复菜单框外的场景; 结束时用 draw_game_over。
 */
static void fish_draw_scene(void) {
    uint8_t i;
    draw_background();
    g_full_redraw = 0;
    draw_image_sprite(FLASH_STORE_ID_PLAYER_0, player_imgs_dim,
        PLAYER_COUNT, g_player.level, g_player.x, g_player.y, g_player.facing);
    g_player.prev_x = (int16_t)g_player.x;
    g_player.prev_y = (int16_t)g_player.y;
    g_player.prev_level = g_player.level;
    g_player.drawn = 1;
    for (i = 0; i < FISH_NUM_FOOD; i++) {
        FishEntity* f = &g_food[i];
        if (!f->active) continue;
        draw_image_sprite(FLASH_STORE_ID_FISH_0, fish_imgs_dim,
            FISH_COUNT, f->level, f->x, f->y, f->facing);
        f->prev_x = (int16_t)f->x;
        f->prev_y = (int16_t)f->y;
        f->prev_level = f->level;
        f->drawn = 1;
    }
    g_hud_dirty = 1;
    draw_hud();
}

/**
 * @brief  恢复菜单框外的游戏场景(玩家/小鱼/背景)。同帧执行, 避免菜单切换/取消时黑闪一帧。
 */
static void fish_restore_scene_behind(void) {
    if (g_state_before_menu == FISH_STATE_OVER) draw_game_over();
    else fish_draw_scene();
    g_full_redraw = 0;
}
/**
 * @brief  初始化鱼游戏: 复位状态、背景色、生成玩家与小鱼并绘制。
 */
void fish_init(void) {
    uint8_t i;
    g_state = FISH_STATE_PLAYING;
    g_score = 0;
    g_won = 0;
    g_menu_clear_color = COLOR_BG;   /* 清菜单位置用背景色替代黑色, 避免切菜单黑闪 */
    g_hud_dirty = 1;   /* 首帧重绘顶部信息 */
    g_frame = 0;
    g_food_us = GetTimerCount();   /* 小鱼时间节拍基准 */
    g_full_redraw = 1;
    g_prev_pressed = 0;
    g_touch_down = 0;
    g_menu_cooldown = 0;
    g_exit_menu_visible = 0;
    g_restart_menu_visible = 0;
    g_restart_sliding = 0;
    g_restart_dragging = 0;
    g_restart_slide_y = RESTART_MENU_Y + RESTART_MENU_H;
    g_player.active = 1;
    g_player.level = FISH_PLAYER_START_LEVEL;
    g_player.x = FISH_PLAYER_X;
    g_player.y = FISH_PLAYER_Y;
    g_player.vx = 0;
    g_player.vy = 0;
    g_player.prev_x = FISH_PLAYER_X;
    g_player.prev_y = FISH_PLAYER_Y;
    g_player.prev_level = FISH_PLAYER_START_LEVEL;
    g_player.drawn = 0;
    g_player.facing = 0;

    for (i = 0; i < FISH_NUM_FOOD; i++)
        food_respawn(&g_food[i], 1);
}

/**
 * @brief  关闭/取消菜单后置脏以便重绘鱼场景。
 * @note   菜单关闭/取消, 及重菜下滑打断进退出菜单(on_to_exit)后立即重绘场景(结束画面/游戏画面);
 *         on_to_exit 与 mark_dirty 行为一致(都仅恢复场景), 故 cfg 两个槽位共用此函数省 Flash。
 */
static void fish_menu_mark_dirty(void) {
    fish_restore_scene_behind();
}

/**
 * @brief  依据脏标记重绘鱼菜单背景场景。
 * @return 1=已重绘, 0=无脏
 */
static uint8_t fish_menu_bg_redraw(void) {
    if (!g_full_redraw) return 0;
    g_full_redraw = 0;
    /* 菜单打开时低电触发 g_full_redraw: 菜单框架不透明, 重绘它盖住中央的低电图标,
       不要整屏重绘场景, 否则菜单会闪 (参考 sokoban 菜单可见时仅重绘菜单框架)。
       凸起区 x=195~210 超出菜单框架右缘(195), 先用鱼背景色填掉避免红色残留 */
    LCD_Fill(195, 79, 211, 162, COLOR_BG);
    return 1;
}

static const GameMenuCfg g_fish_menu_cfg = {
    FISH_STATE_PLAYING, FISH_STATE_EXIT_MENU, FISH_STATE_RESTART_MENU,
    fish_row_types, fish_exit_map,
    220, 10,
    GAME_MENU_BAT_NONE, 0,
    fish_menu_mark_dirty, fish_menu_bg_redraw, fish_menu_mark_dirty
};
/**
 * @brief  处理玩家鱼的拖动(仅在"游戏中且手指按下"时调用)。
 * @param  ts           当前触摸状态
 * @param  just_pressed 本帧是否刚按下
 * @note   抽成独立函数是为了给 fish_update 瘦身: 单函数代码过长会让内部短跳转超范围
 *         (R_RISCV_BRANCH / R_RISCV_RVC_JUMP truncated to fit)。
 *         位置用指数平滑(既防手抖又不产生台阶); 朝向用"累积位移"判定(换向后清零, 反向灵敏)。
 */
static void fish_update_drag(const TouchState* ts, uint8_t just_pressed)
{
    if (g_game_consume_release) {
        /* 菜单反向滑关闭后的拖动不传入游戏: 闭合手势按住期间只跟随不移动鱼, 松手即恢复 */
        s_drag_start_fx = (int16_t)g_player.x;
        s_drag_start_fy = (int16_t)g_player.y;
        s_drag_start_tx = (int16_t)ts->x;
        s_drag_start_ty = (int16_t)ts->y;
        s_prev_px = (int16_t)g_player.x;
        s_face_accum = 0;
    }
    else if (just_pressed) {
        /* 刚按下: 记录拖动起点(鱼位置+手指位置), 本帧鱼不动 */
        s_drag_start_fx = (int16_t)g_player.x;
        s_drag_start_fy = (int16_t)g_player.y;
        s_drag_start_tx = (int16_t)ts->x;
        s_drag_start_ty = (int16_t)ts->y;
        s_prev_px = (int16_t)g_player.x;
        s_face_accum = 0;
    }
    else {
        /* 绝对位置法(与菜单拖动同理): 目标 = 拖动起点的鱼位置 + 手指总位移,
           不累加逐帧增量, 因此没有累积误差。 */
        int16_t target_x = (int16_t)(s_drag_start_fx + ((int16_t)ts->x - s_drag_start_tx));
        int16_t target_y = (int16_t)(s_drag_start_fy + ((int16_t)ts->y - s_drag_start_ty));

        /* 位置: 指数平滑(每帧向目标靠近 1/FISH_SMOOTH_DIV) + "最大滞后"上限。
           - 静止: 手抖是"来回摆", 平滑后被平均掉, 鱼基本不动;
           - 滑动: 无死区、无门槛, 不会"隔几像素才动"的台阶;
           - 快速滑动: 只按 1/3 靠近时, 手一挥(一两帧走完)鱼只走到一半就停在半路
             (实测"快速滑动很长距离, 鱼只移动大概一半"), 故加一条上限: 与目标的距离
             超过 FISH_MAX_LAG 时, 超出部分直接走掉, 只保留 FISH_MAX_LAG 的滞后。
             小幅抖动(距离 <= FISH_MAX_LAG)仍按 1/3 平滑, 去抖效果不变。 */
        {
            int16_t fx = (int16_t)(target_x - (int16_t)g_player.x);
            int16_t fy = (int16_t)(target_y - (int16_t)g_player.y);
            int16_t sx = (int16_t)(fx / FISH_SMOOTH_DIV);
            int16_t sy = (int16_t)(fy / FISH_SMOOTH_DIV);

            if (fx > FISH_MAX_LAG)        sx = (int16_t)(fx - FISH_MAX_LAG);
            else if (fx < -FISH_MAX_LAG)  sx = (int16_t)(fx + FISH_MAX_LAG);
            if (fy > FISH_MAX_LAG)        sy = (int16_t)(fy - FISH_MAX_LAG);
            else if (fy < -FISH_MAX_LAG)  sy = (int16_t)(fy + FISH_MAX_LAG);

            g_player.x = (uint16_t)((int16_t)g_player.x + sx);
            g_player.y = (uint16_t)((int16_t)g_player.y + sy);
        }

        /* 朝向: 累积"平滑后位置"的单向位移, 攒够 FISH_FACE_DEAD 才换向。
           - 不用"相对起点的净位移": 反向时要从 +N 退回负值, 转向会很迟钝;
           - 换向后累积清零 → 反向只需再走几像素就转向, 灵敏;
           - 静止时平滑位置几乎不变, 累积不增长 → 朝向不会来回翻。 */
        {
            int16_t step = (int16_t)((int16_t)g_player.x - s_prev_px);
            s_face_accum = (int16_t)(s_face_accum + step);
            if (s_face_accum >= FISH_FACE_DEAD) {
                g_player.facing = 0;      /* 向右 */
                s_face_accum = 0;
            }
            else if (s_face_accum <= -FISH_FACE_DEAD) {
                g_player.facing = 1;      /* 向左 */
                s_face_accum = 0;
            }
        }
        s_prev_px = (int16_t)g_player.x;
    }

    /* 限制在8角形游动区(矩形+四角倒角, 不进圆形屏被遮挡的四角) */
    {
        int16_t px = (int16_t)g_player.x;
        int16_t py = (int16_t)g_player.y;
        fish_clamp_pos(&px, &py);
        g_player.x = (uint16_t)px;
        g_player.y = (uint16_t)py;
    }
}

/**
 * @brief  鱼每帧更新: 处理拖动、小鱼移动/吃鱼、菜单交互与渲染。
 * @return GAME_RET_* 返回命令
 */
uint8_t fish_update(void) {

    /* 菜单冷却 */
    if (g_menu_cooldown > 0) g_menu_cooldown--;

    /* 设置页面下滑→退出菜单 */
    if (g_game_enter_exit_menu) {
        g_game_enter_exit_menu = 0;
        /* 这条路没经过 game_menu_enter_exit_drag, 需自行记录"进菜单前的状态":
           fish_restore_scene_behind 与关闭菜单时的 *state 恢复都读它 */
        g_state_before_menu = g_state;
        g_state = FISH_STATE_EXIT_MENU;
        /* 设置页是整屏画上去的(背景已不是游戏画面), 必须同帧整屏重绘鱼场景 ——
           不能只置 g_full_redraw: fish_menu_bg_redraw 在菜单态只补凸起区(防菜单闪),
           不重绘场景, 否则退出菜单下面是黑的(推箱子同处已直接 sokoban_draw_level)。 */
        fish_restore_scene_behind();
    }

    /* ---- 退出菜单 / 重新开始菜单: 配置驱动统一状态机 ---- */
    if (g_state == FISH_STATE_EXIT_MENU || g_state == FISH_STATE_RESTART_MENU) {
        uint8_t out_ret;
        uint8_t cmd = game_menu_command(&g_fish_menu_cfg, (uint8_t*)&g_state, &out_ret);
        switch (cmd) {
        case GAME_MENU_CMD_SWITCH:   return out_ret;
        case GAME_MENU_CMD_RESTART:  fish_init(); return GAME_RET_INTERACT;
        case GAME_MENU_CMD_SETTINGS: menu_enter_game_settings(); return EXIT_RET_SETTINGS;
        case GAME_MENU_CMD_INTERACT: return GAME_RET_INTERACT;
        default:                     return 0;  /* RETURN/关闭 */
        }
    }

    /* ---- 游戏结束: 上滑/下滑呼出菜单 (不再点击任意处重开) ---- */
    if (g_state == FISH_STATE_OVER) {
        if (g_full_redraw) { draw_game_over(); g_full_redraw = 0; }
        TouchState ts;
        Touch_Scan(&ts);
        if (ts.pressed && !g_touch_down) {
            g_touch_down = 1;
            g_touch_start_x = ts.x;
            g_touch_start_y = ts.y;
            if (ts.y <= 220) g_menu_cooldown = 0;  /* 非上划区清菜单冷却 */
        }
        else if (ts.pressed && g_touch_down && g_touch_start_y < MENU_SWIPE_START_Y) {
            int16_t dy_move = (int16_t)ts.y - (int16_t)g_touch_start_y;
            if (dy_move > MENU_SWIPE_TRIGGER_DIST) {
                g_state_before_menu = g_state;
                g_state = FISH_STATE_EXIT_MENU;
                g_exit_menu_visible = 1;
                g_exit_dragging = 1;
                g_exit_drag_start_y = ts.y;
                if (dy_move > MENU_DRAG_MAX_STEP) dy_move = MENU_DRAG_MAX_STEP;
                g_exit_drag_y = -MENU_H + dy_move;
                return 0;
            }
        }
        else if (ts.pressed && g_touch_down && g_touch_start_y > 220) {
            int16_t dy_move = (int16_t)ts.y - (int16_t)g_touch_start_y;
            if (dy_move < -5) {
                g_state_before_menu = g_state;
                g_state = FISH_STATE_RESTART_MENU;
                g_restart_menu_visible = 1;
                g_restart_dragging = 1;
                g_restart_sliding = 0;
                g_restart_slide_y = RESTART_MENU_Y + RESTART_MENU_H;
                game_restart_menu_drag_poll(&g_restart_slide_y, g_touch_start_y,
                    &g_restart_dragging, &ts);
                return 0;
            }
        }
        else if (!ts.pressed && g_touch_down) {
            g_touch_down = 0;
            int16_t dy = (int16_t)ts.y - (int16_t)g_touch_start_y;
            if (g_touch_start_y > 220 && dy < -10) {
                g_state_before_menu = g_state;
                g_state = FISH_STATE_RESTART_MENU;
                g_restart_menu_visible = 1;
                g_restart_sliding = 1;
                g_restart_dragging = 0;
                g_restart_slide_y = RESTART_MENU_Y + RESTART_MENU_H + dy;
                if (g_restart_slide_y < RESTART_MENU_Y + RESTART_MENU_RELEASE_GAP)
                    g_restart_slide_y = RESTART_MENU_Y + RESTART_MENU_RELEASE_GAP;
                return GAME_RET_INTERACT;
            }
            if (g_touch_start_y < MENU_SWIPE_START_Y && dy >= MENU_SWIPE_COMMIT_DIST) {
                g_state_before_menu = g_state;
                g_state = FISH_STATE_EXIT_MENU;
                g_exit_menu_visible = 1;
                g_exit_sliding = 1;
                g_exit_drag_y = -MENU_H;
                return GAME_RET_INTERACT;
            }
        }
        g_prev_pressed = ts.pressed;
        return GAME_RET_INTERACT;
    }

    TouchState ts;
    Touch_Scan(&ts);
    g_frame++;

    uint8_t upress = ts.pressed;
    uint8_t just_pressed = (uint8_t)(upress && !g_prev_pressed);
    g_prev_pressed = upress;

    /* 触摸起始记录 (供菜单手势检测) */
    if (upress && !g_touch_down) {
        g_touch_down = 1;
        g_touch_start_x = ts.x;
        g_touch_start_y = ts.y;
        if (ts.y <= 220) g_menu_cooldown = 0;  /* 非上划区清菜单冷却 */
    }
    /* 下滑触发退出菜单拖拽 */
    if (game_menu_enter_exit_drag(&ts, (uint8_t*)&g_state,
        FISH_STATE_PLAYING, FISH_STATE_EXIT_MENU))
        return 0;
    /* 上划触发重新开始菜单 */
    if (game_menu_enter_restart_drag(&ts, (uint8_t*)&g_state,
        FISH_STATE_PLAYING, FISH_STATE_RESTART_MENU, 220))
        return 0;
    /* 松手提交菜单 */
    if (!upress && g_touch_down) {
        g_touch_down = 0;
        int16_t dy = (int16_t)ts.y - (int16_t)g_touch_start_y;
        if (game_menu_commit_exit(dy, (uint8_t*)&g_state, FISH_STATE_EXIT_MENU))
            return GAME_RET_INTERACT;
        if (game_menu_commit_restart(dy, (uint8_t*)&g_state,
            FISH_STATE_RESTART_MENU, 220, 10))
            return GAME_RET_INTERACT;
    }
    /* 拖动: 按住时玩家鱼跟手移动(实现见 fish_update_drag, 抽出去是给本函数瘦身) */
    if (g_state == FISH_STATE_PLAYING && upress)
        fish_update_drag(&ts, just_pressed);

    /* 闭合手势结束(松手)后清除吸收标志, 下一次按下即可正常操作游戏 */
    if (!upress && g_game_consume_release)
        g_game_consume_release = 0;

    /* 小鱼随机游动 (基于真实时间节拍, 与帧率无关, 防未按下时主循环空转导致狂飙) */
    {
        uint32_t now_us = GetTimerCount();
        if ((uint32_t)(now_us - g_food_us) >= FISH_MOVE_US) {
            g_food_us = now_us;
            uint8_t i;
            for (i = 0; i < FISH_NUM_FOOD; i++) {
                FishEntity* f = &g_food[i];
                if (!f->active) continue;
                /* 走出屏幕中: 直冲出屏, 完全出屏后换新鱼 */
                if (f->leaving) {
                    f->x = (uint16_t)((int16_t)f->x + f->vx);
                    f->y = (uint16_t)((int16_t)f->y + f->vy);
                    {
                        uint16_t w = fish_imgs_dim[f->level][0];
                        uint16_t h = fish_imgs_dim[f->level][1];
                        if ((int16_t)f->x + (int16_t)(w / 2) < 0 ||
                            (int16_t)f->x - (int16_t)(w / 2) > (int16_t)LCD_W ||
                            (int16_t)f->y + (int16_t)(h / 2) < 0 ||
                            (int16_t)f->y - (int16_t)(h / 2) > (int16_t)LCD_H) {
                            food_respawn(f, 1);
                        }
                    }
                    continue;
                }

                /* 从屏幕边缘游入: 不钳制, 游进可游动区后转为常规移动 */
                if (f->entering) {
                    f->x = (uint16_t)((int16_t)f->x + f->vx);
                    f->y = (uint16_t)((int16_t)f->y + f->vy);
                    if ((int16_t)f->x >= (int16_t)PLAY_X_MIN && (int16_t)f->x <= (int16_t)PLAY_X_MAX &&
                        (int16_t)f->y >= (int16_t)PLAY_Y_MIN && (int16_t)f->y <= (int16_t)PLAY_Y_MAX) {
                        f->entering = 0;
                    }
                    continue;
                }

                f->x = (uint16_t)((int16_t)f->x + f->vx);
                f->y = (uint16_t)((int16_t)f->y + f->vy);
                if (f->x < PLAY_X_MIN) { f->x = PLAY_X_MIN; f->vx = (int16_t)-f->vx; }
                if (f->x > PLAY_X_MAX) { f->x = PLAY_X_MAX; f->vx = (int16_t)-f->vx; }
                if (f->y < PLAY_Y_MIN) { f->y = PLAY_Y_MIN; f->vy = (int16_t)-f->vy; }
                if (f->y > PLAY_Y_MAX) { f->y = PLAY_Y_MAX; f->vy = (int16_t)-f->vy; }
                /* 8角形钳制: 不进圆形屏被遮挡的四角 */
                {
                    int16_t fx = (int16_t)f->x;
                    int16_t fy = (int16_t)f->y;
                    fish_clamp_pos(&fx, &fy);
                    f->x = (uint16_t)fx;
                    f->y = (uint16_t)fy;
                }
                /* 角落反弹: 速度朝外时反转向内, 防卡角 */
                fish_bounce_corners(f);
                /* 偶尔改变方向 */
                if (fish_rand(64) == 0) {
                    f->vx = (int16_t)fish_rand(FISH_SPEED_BASE * 2 + 1) - FISH_SPEED_BASE;
                    f->vy = (int16_t)fish_rand(FISH_SPEED_BASE * 2 + 1) - FISH_SPEED_BASE;
                    if (f->vx == 0 && f->vy == 0) f->vx = 1;   /* 避免停死 */
                }
                /* 按游动方向更新朝向(左右翻转) */
                if (f->vx != 0) f->facing = (f->vx < 0) ? 1 : 0;
                /* 寿命到期: 朝最近边缘游出屏幕(不原地消失) */
                f->life++;
                if (f->life >= ((f->level <= g_player.level) ? FISH_LIFE_TICKS : FISH_LIFE_BIG_TICKS)) {
                    f->leaving = 1;
                    int16_t dl = (int16_t)f->x;
                    int16_t dr = (int16_t)LCD_W - (int16_t)f->x;
                    int16_t dt = (int16_t)f->y;
                    int16_t db = (int16_t)LCD_H - (int16_t)f->y;
                    if (dl < dr && dl < dt && dl < db) { f->vx = -FISH_LEAVE_SPEED; f->vy = 0; }
                    else if (dr < dt && dr < db) { f->vx = FISH_LEAVE_SPEED;  f->vy = 0; }
                    else if (dt < db) { f->vx = 0;                 f->vy = -FISH_LEAVE_SPEED; }
                    else { f->vx = 0;                 f->vy = FISH_LEAVE_SPEED; }
                    /* 离场朝最近边缘游出, 同步朝向往出口方向 */
                    if (f->vx != 0) f->facing = (f->vx < 0) ? 1 : 0;
                }
            }
        }
    }

    /* 碰撞检测: 玩家 vs 小鱼 */
    {
        uint8_t i;
        for (i = 0; i < FISH_NUM_FOOD; i++) {
            FishEntity* f = &g_food[i];
            if (!f->active) continue;
            int16_t ddx = (int16_t)f->x - (int16_t)g_player.x;
            int16_t ddy = (int16_t)f->y - (int16_t)g_player.y;
            if (ddx < 0) ddx = (int16_t)-ddx;
            if (ddy < 0) ddy = (int16_t)-ddy;
            /* 粗略判定: 玩家尺寸随等级增大 */
            uint16_t pr = (uint16_t)(8 + PLAYER_IMG_LVL * 4);
            if (ddx < (int16_t)pr && ddy < (int16_t)pr) {
                if (f->level <= g_player.level ||
                    (g_player.level >= (FISH_PLAYER_LEVEL_MAX - 1) && f->level <= (uint8_t)(g_player.level + 1))) {
                    /* 吃掉: 按等级得分(5^等级:0级=1,1级=5...), 累计分达阈值即升一级 */
                    g_score += fish_thresh(f->level);
                    g_hud_dirty = 1;   /* 顶部分数/进度需刷新 */
                    while (g_player.level < FISH_PLAYER_LEVEL_MAX &&
                        g_score >= fish_thresh((uint8_t)(g_player.level + 1))) {
                        g_player.level++;   /* 累计分达到下一级阈值, 升一级 */
                    }
                    food_respawn(f, 1);
                    /* 达到1,953,125分(显示10级): 通关(胜利) */
                    if (g_score >= FISH_WIN_SCORE) {
                        g_won = 1;
                        g_state = FISH_STATE_OVER;
                        draw_game_over();
                        g_touch_down = 0;
                        return GAME_RET_INTERACT;
                    }
                }
                else if (!FISH_INVINCIBLE) {
                    /* 碰到更大的鱼: 游戏结束 */
                    g_state = FISH_STATE_OVER;
                    draw_game_over();
                    g_touch_down = 0;
                    return GAME_RET_INTERACT;
                }
            }
        }
    }

    /* 增量绘制 (不整屏重绘, 只刷新有变化的鱼/玩家, 防闪烁) */
    if (g_full_redraw) {
        /* 首次/恢复: 全屏填背景, 所有实体重画 */
        draw_background();
        g_full_redraw = 0;
        g_hud_dirty = 1;      /* 顶部分数/等级条被背景覆盖, 需重绘 */
        draw_hud();
        g_player.drawn = 0;
        uint8_t i0;
        for (i0 = 0; i0 < FISH_NUM_FOOD; i0++) g_food[i0].drawn = 0;
    }

    /* 玩家: 位置/等级变化 或 被重叠误伤标记时才重画 */
    {
        FishEntity* p = &g_player;
        uint8_t moved = (p->x != (uint16_t)p->prev_x ||
            p->y != (uint16_t)p->prev_y ||
            p->level != p->prev_level);
        if (!p->drawn || moved) {
            if (p->drawn && moved) {
                clear_entity_rect(player_imgs_dim, p->prev_level,
                    p->prev_x, p->prev_y);
                /* 等级变化(尺寸变大)时, 新尺寸矩形也清一次背景, 避免残留旧图 */
                if (p->level != p->prev_level)
                    clear_entity_rect(player_imgs_dim, PLAYER_IMG_LVL,
                        (int16_t)p->x, (int16_t)p->y);
            }
            draw_image_sprite(FLASH_STORE_ID_PLAYER_0, player_imgs_dim,
                PLAYER_COUNT, p->level, p->x, p->y, p->facing);
            p->prev_x = (int16_t)p->x;
            p->prev_y = (int16_t)p->y;
            p->prev_level = p->level;
            p->drawn = 1;
        }
    }

    /* 小鱼: 逻辑同玩家 */
    {
        uint8_t i;
        for (i = 0; i < FISH_NUM_FOOD; i++) {
            FishEntity* f = &g_food[i];
            if (!f->active) continue;
            uint8_t moved = (f->x != (uint16_t)f->prev_x ||
                f->y != (uint16_t)f->prev_y ||
                f->level != f->prev_level);
            if (!f->drawn || moved) {
                if (f->drawn && moved)
                    fish_move_redraw(f);   /* 行级原子重绘(鱼移动), 减闪 */
                else
                    draw_image_sprite(FLASH_STORE_ID_FISH_0, fish_imgs_dim,
                        FISH_COUNT, f->level, f->x, f->y, f->facing);
                f->prev_x = (int16_t)f->x;
                f->prev_y = (int16_t)f->y;
                f->prev_level = f->level;
                f->drawn = 1;
            }
        }
    }

    /* 顶部信息 */
    draw_hud();

    return GAME_RET_NONE;
}

/**
 * @brief  复位鱼游戏触摸状态(从其它游戏返回时调用)。
 */
void fish_reset_touch(void) {
    g_prev_pressed = 0;
    s_drag_start_tx = 0;
    s_drag_start_ty = 0;
}

/**
 * @brief  请求鱼游戏全屏重绘(电池闪灭/菜单切换后恢复)。
 */
void fish_full_redraw(void) {
    g_full_redraw = 1;
}

/**
 * @brief 局部清除低电图标覆盖区, 不整屏重绘(避免游戏画面闪)
 * @note  菜单可见时交由菜单状态机重绘框架盖住; 游戏界面复用
 *        clear_entity_rect 填背景并重绘相交实体, 故顶部分数/等级不受影响
 */
void fish_clear_lowbattery(void) {
    /* 低电图标覆盖区: 中心(127,120), 宽166, 高83 (x=44~209, y=79~161) */
    static const uint16_t lb_dims[][2] = { {166, 83} };

    /* ★ 设置模式(二级设置条带压在游戏上): 屏幕是"游戏+设置条带", 条带已盖住电池主体,
       只恢复电池矩形(含条带外的凸点)内的实体即可 —— 不能走下面的菜单分支(会把游戏
       菜单画到设置条带上), 也不能整屏重绘(会闪, 见 §16 鱼的坑) */
    if (g_settings_from_game) {
        fish_erase_lowbatt_icon();                      /* 用背景色擦掉图标线条 */
        redraw_entities_in_rect(lb_dims, 0, 127, 120);  /* 补回压在线条上的鱼/玩家 */
        return;
    }

    if (g_state == FISH_STATE_EXIT_MENU || g_state == FISH_STATE_RESTART_MENU) {
        /* 区域刷新(不整屏重绘, 避免游戏画面闪): 清红色低电图标后, 填电池区为背景色
           并重绘与其相交的玩家/小鱼实体, 再同帧重画菜单框架. 菜单拖拽/滑入中(未到位)
           时框外露出部分得以恢复, 不残留红/黑不闪; 到位时框内部分由菜单框架盖住. */
        /* 菜单背后是"游戏结束"画面(g_state_before_menu==OVER)时, 不能重绘小鱼(会盖掉结束文字),
           改为图标区填背景+重绘结束文字(不整屏清黑, 避免闪); 否则按透明实体场景区域恢复 */
        if (g_state_before_menu == FISH_STATE_OVER) {
            LCD_Fill_DMA(44, 79, 210, 162, COLOR_BG);   /* 填电池区(走 DMA 提速) */
            draw_game_over_text();
        }
        else {
            /* 只擦图标线条本身(用背景色), 不整块填背景 —— 整块填会把框内的鱼/玩家
               一起擦掉再重画, 而重画要从外部 Flash 流式读精灵图, 那期间框内是空的,
               用户看到"电池框内部黑一下" */
            fish_erase_lowbatt_icon();
            redraw_entities_in_rect(lb_dims, 0, 127, 120);
        }
        if (g_state == FISH_STATE_EXIT_MENU)
            menu_draw_nrow_frame_only(g_exit_drag_y, fish_row_types, 5);
        else
            menu_draw_nrow_frame_only(g_restart_slide_y, g_restart_row_types, 3);
        return;
    }
    /* 游戏结束(无菜单): 图标区填背景(清图标)+重绘结束文字, 不整屏清黑, 避免闪 */
    if (g_state == FISH_STATE_OVER) {
        LCD_Fill_DMA(44, 79, 210, 162, COLOR_BG);   /* 填电池区(走 DMA 提速) */
        draw_game_over_text();
    }
    else {
        fish_erase_lowbatt_icon();                      /* 用背景色擦掉图标线条 */
        redraw_entities_in_rect(lb_dims, 0, 127, 120);  /* 补回压在线条上的鱼/玩家 */
    }
}

/* 存档 / 恢复: 存玩家等级/坐标/朝向 + 完整得分/状态 + 每条小鱼位置/速度。
   恢复时原地继续, 避免"鱼被重新生成到屏幕外边缘、短暂无鱼"的问题 */
   /**
    * @brief  序列化鱼游戏状态(玩家+每只小鱼)到存档缓冲。
    * @param  buf     输出缓冲
    * @param  max_len 缓冲上限
    * @return 写入字节数
    */
uint8_t fish_save_state(uint8_t* buf, uint8_t max_len) {
    uint8_t i, state;
    uint8_t* p;
    if (!buf || max_len < FISH_SAVE_LEN) return 0;

    /* 菜单打开时存进入菜单前的状态 */
    if (g_state == FISH_STATE_EXIT_MENU || g_state == FISH_STATE_RESTART_MENU)
        state = g_state_before_menu;
    else
        state = g_state;
    if (state > FISH_STATE_OVER) state = FISH_STATE_PLAYING;

    p = buf;
    *p++ = g_player.level;
    p[0] = (uint8_t)(g_player.x & 0xFF); p[1] = (uint8_t)(g_player.x >> 8); p += 2;
    p[0] = (uint8_t)(g_player.y & 0xFF); p[1] = (uint8_t)(g_player.y >> 8); p += 2;
    *p++ = g_player.facing;
    *p++ = (uint8_t)(g_score & 0xFF);
    *p++ = (uint8_t)((g_score >> 8) & 0xFF);
    *p++ = (uint8_t)((g_score >> 16) & 0xFF);
    *p++ = (uint8_t)((g_score >> 24) & 0xFF);
    *p++ = state;

    for (i = 0; i < FISH_NUM_FOOD; i++) {
        const FishEntity* f = &g_food[i];
        *p++ = f->active;
        *p++ = f->level;
        p[0] = (uint8_t)(f->x & 0xFF); p[1] = (uint8_t)(f->x >> 8); p += 2;
        p[0] = (uint8_t)(f->y & 0xFF); p[1] = (uint8_t)(f->y >> 8); p += 2;
        p[0] = (uint8_t)(f->vx & 0xFF); p[1] = (uint8_t)((uint16_t)f->vx >> 8); p += 2;
        p[0] = (uint8_t)(f->vy & 0xFF); p[1] = (uint8_t)((uint16_t)f->vy >> 8); p += 2;
    }
    return FISH_SAVE_LEN;
}
/**
 * @brief  从存档缓冲恢复鱼游戏状态并重绘。
 * @param  buf 存档缓冲
 * @param  len 长度
 * @return 1=成功, 0=失败
 */
uint8_t fish_restore_state(const uint8_t* buf, uint8_t len) {
    uint8_t i, state;
    const uint8_t* p;
    uint32_t score;

    if (!buf || len < FISH_SAVE_LEN) return 0;
    p = buf;

    g_player.level = *p++;
    g_player.x = (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); p += 2;
    g_player.y = (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); p += 2;
    g_player.facing = *p++;
    score = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
        | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    p += 4;
    state = *p++;
    if (state > FISH_STATE_OVER) return 0;

    g_score = score;
    g_won = (g_score >= FISH_WIN_SCORE) ? 1 : 0;
    g_state = (state == FISH_STATE_OVER) ? FISH_STATE_OVER : FISH_STATE_PLAYING;

    /* 玩家: 恢复坐标, 速度清零(玩家由触摸增量驱动), 其余复位 */
    g_player.active = 1;
    g_player.vx = 0;
    g_player.vy = 0;
    g_player.prev_x = (int16_t)g_player.x;
    g_player.prev_y = (int16_t)g_player.y;
    g_player.prev_level = g_player.level;
    g_player.drawn = 0;
    g_player.leaving = 0;
    g_player.entering = 0;
    g_player.dir = 0;

    /* 每条小鱼: 曾在场则原地恢复(带速度), 空位则重新生成一条 */
    for (i = 0; i < FISH_NUM_FOOD; i++) {
        FishEntity* f = &g_food[i];
        uint8_t act = *p++;
        f->level = *p++;
        f->x = (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); p += 2;
        f->y = (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); p += 2;
        f->vx = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); p += 2;
        f->vy = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8)); p += 2;
        if (act) {
            f->active = 1;
            f->prev_x = (int16_t)f->x;
            f->prev_y = (int16_t)f->y;
            f->prev_level = f->level;
            f->drawn = 0;
            f->leaving = 0;
            f->entering = 0;
        }
        else {
            f->drawn = 0;
            food_respawn(f, 0);
        }
    }

    g_menu_clear_color = COLOR_BG;
    g_hud_dirty = 1;   /* 首帧重绘顶部信息 */
    g_frame = 0;
    g_food_us = GetTimerCount();
    g_full_redraw = 1;
    g_prev_pressed = 0;
    g_touch_down = 0;
    g_menu_cooldown = 0;
    g_exit_menu_visible = 0;
    g_restart_menu_visible = 0;
    g_restart_sliding = 0;
    g_restart_dragging = 0;
    g_restart_slide_y = RESTART_MENU_Y + RESTART_MENU_H;

    /* 立即重绘, 避免恢复后黑屏一帧 */
    LCD_Fill(0, 0, LCD_W, LCD_H, COLOR_BG);
    if (g_state == FISH_STATE_OVER) draw_game_over();
    else fish_draw_scene();
    return 1;
}

#ifdef SIM_BUILD
/* ---- sim-only: report fish's playing-state touch regions ---------------- */
int fish_sim_touch_rects(void)
{
    sim_touch_add(0, 0, 240, 240, SIM_TOUCH_SWIPE);              /* finger steers the fish */
    sim_touch_add(0, 0, 240, MENU_SWIPE_START_Y, SIM_TOUCH_BAND);
    sim_touch_add(0, 221, 240, 240, SIM_TOUCH_BAND);             /* y>220: restart menu */
    return 1;
}
#endif