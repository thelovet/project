#include "timer.h"
#include "ch32v20x_pwr.h"
#include "display.h"
#include "battery.h"

static uint32_t fps = 0;
static uint32_t frame_count = 0;
static uint32_t last_time = 0;
static uint32_t start_time = 0;
static volatile uint32_t overflow_count = 0;
static uint32_t max_frame_time = 0;//最大帧耗时，用于最低帧率
static uint32_t instant_fps = 0;//实时帧率，每帧更新
static uint32_t sum_delta = 0;//帧间隔累加
static uint32_t sample_count = 0;//采样帧数
static uint32_t last_fps_update = 0;//上次更新时刻

static volatile uint8_t low_battery_pending = 0;

#define ONE_SECOND  1000000

/**
 * @brief 初始化定时器2（65ms）
 * 
 */
void TIMER2_Init(void)
{
    TIM_TimeBaseInitTypeDef TIM_TimeBaseInitStructure={0};

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_TIM1, ENABLE);

    TIM_TimeBaseInitStructure.TIM_Period = 65535;
    TIM_TimeBaseInitStructure.TIM_Prescaler = 143;
    TIM_TimeBaseInitStructure.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseInitStructure.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInitStructure.TIM_RepetitionCounter = 0;
    TIM_TimeBaseInit(TIM1, &TIM_TimeBaseInitStructure);

    TIM_ClearITPendingBit(TIM1, TIM_IT_Update);
    TIM_ITConfig(TIM1, TIM_IT_Update, ENABLE);

    NVIC_InitTypeDef NVIC_InitStructure={0};
    NVIC_InitStructure.NVIC_IRQChannel = TIM1_UP_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 2;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 1;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    TIM_Cmd(TIM1, ENABLE);
}


/**
 * @brief 定时器中断服务函数（每65ms触发一次）
 * 
 */
void TIM1_UP_IRQHandler(void)
    __attribute__((interrupt("WCH-Interrupt-fast"), used, externally_visible));
void TIM1_UP_IRQHandler(void)
{
    if(TIM_GetITStatus(TIM1, TIM_IT_Update) == SET)
    {
        TIM_ClearITPendingBit(TIM1, TIM_IT_Update);
        overflow_count++;

        // 每次溢出检测电池低压（~65ms/次），充电时跳过
        if (!Battery_IsCharging() && Battery_GetStatus() != BAT_FULL && PWR_GetFlagStatus(PWR_FLAG_PVDO) == SET) {//仅在电池低压且未充电时检测
            low_battery_pending = 1;//交给主循环处理，禁止在中断中绘屏和延时
        }
    }
}

/**
 * @brief 获取定时器计数（tick）
 * 
 * @return uint32_t 定时器计数（tick）
 */
uint8_t TIMER2_TakeLowBatteryPending(void)
{
    uint8_t pending = low_battery_pending;
    low_battery_pending = 0;
    return pending;
}

uint32_t GetTimerCount(void)//获取定时器计数
{
    uint32_t high_before;
    uint32_t high_after;
    uint16_t low;
    FlagStatus update_pending;

    /*
     * 不读取mstatus，避免部分唤醒状态下访问该CSR触发非法指令。
     * 若读取期间溢出中断改变了高位计数，则重新读取；若更新标志
     * 已置位但ISR尚未执行，则补偿本次溢出，防止时间值瞬间倒退。
     */
    do {
        high_before = overflow_count;
        low = (uint16_t)TIM1->CNT;
        update_pending = TIM_GetFlagStatus(TIM1, TIM_FLAG_Update);
        high_after = overflow_count;
    } while (high_before != high_after);

    if (update_pending != RESET) {
        high_before++;
        low = (uint16_t)TIM1->CNT;
    }

    return (high_before << 16) | low;
}

/**
 * @brief 触发帧计数（每帧调用一次）
 * 
 */
void FrameTrigger(void)//触发帧计数
{
    uint32_t current_time = GetTimerCount();
    uint32_t delta_time = current_time - last_time;
    
    frame_count++;
    last_time = current_time;
    
    // 累加帧间隔，用于0.5S平均
    if (delta_time > 0 && delta_time < ONE_SECOND) {
        sum_delta += delta_time;
        sample_count++;
        if (instant_fps == 0) {//首个有效帧立即显示，不等0.5S
            instant_fps = 1000000 / delta_time;
        }
    }
    
    if (start_time == 0) {//首次调用，仅初始化计时起点
        start_time = current_time;
        last_fps_update = current_time;
        return;
    }
    
    // 0.5S更新一次帧率，避免跳动
    if (current_time - last_fps_update >= 500000) {
        if (sample_count > 0 && sum_delta > 0) {
            instant_fps = (1000000 * sample_count) / sum_delta;
        }
        sum_delta = 0;
        sample_count = 0;
        last_fps_update = current_time;
    }
    
    uint32_t elapsed = current_time - start_time;
    
    if(elapsed >= ONE_SECOND)
    {
        fps = frame_count;
        frame_count = 0;
        start_time = current_time;
    }
}

/**
 * @brief 获取实时帧率（每帧更新）
 * 
 * @return uint32_t 实时帧率（tick）
 */
uint32_t GetFPS(void)//获取实时帧率
{
    return instant_fps;
}

/**
 * @brief 获取最低帧率（每帧取样，基于最大帧间隔）
 * 
 * @return uint32_t 最低帧率（tick）
 */
uint32_t GetLowestFPS(void)//获取最低帧率（每帧取样，基于最大帧间隔）
{
    if (max_frame_time == 0) return 0;
    return 1000000 / max_frame_time;  // 1 tick = 1us, 最慢帧耗时 = 最低帧率
}


/**
 * @brief 外部更新最大帧时间（由实际帧耗时驱动）
 * 
 * @param duration 实际帧耗时（tick）
 */
void UpdateMaxFrameTime(uint32_t duration)//外部更新最大帧时间（由实际帧耗时驱动）
{
    if (duration > max_frame_time) max_frame_time = duration;
}


/**
 * @brief 重置帧率计数（休眠唤醒后调用）
 * 
 */
void ResetFPS(void)//重置帧率计数
{
    fps = 0;
    frame_count = 0;
    overflow_count = 0;
    start_time = 0;
    max_frame_time = 0;
}

/**
 * @brief 重置最低帧率（休眠唤醒后调用）
 * 
 */
void ResetLowestFPS(void)//重置最低帧率（休眠唤醒后调用）
{
    max_frame_time = 0;
}
