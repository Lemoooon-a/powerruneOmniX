/**
 * @file PowerRune_Armour.cpp
 * @brief 装甲板类
 * @version 0.2
 * @date 2024-02-19
 * @note 装甲板类，用于装甲板控制
 */
#include "PowerRune_Armour.h"
static const char *TAG_ARMOUR = "Armour";
// 变量初始化
LED_Strip *PowerRune_Armour::led_strip[5];
SemaphoreHandle_t PowerRune_Armour::ISR_mutex = xSemaphoreCreateBinary();
DEMUX PowerRune_Armour::demux_led = DEMUX(DEMUX_IO, DEMUX_IO_enable);
TaskHandle_t PowerRune_Armour::LED_update_task_handle;
SemaphoreHandle_t PowerRune_Armour::LED_Strip_FSM_Semaphore;
LED_Strip_FSM_t PowerRune_Armour::state;

bool valid[10] = {true, true, true, true, true, true, true, true, true, true};
static volatile bool pending_blink_after_hit = false;
static volatile uint8_t big_rune_group_index = 1;
static volatile uint8_t big_rune_target_armed = 1;
static volatile uint8_t big_rune_progress_stage = 0;

constexpr static uint8_t BIG_RUNE_PROGRESS_STAGE_TOTAL = 5;
constexpr static uint16_t MATRIX_LED_COUNT = 165;
constexpr static uint16_t ARM_LED_COUNT = 82;
constexpr static uint16_t ARM_BOTTOM_START_INDEX = 36; // arm布局: 18+18+10+18+18, 底边10灯起点
constexpr static uint16_t ARM_BOTTOM_LED_COUNT = 10;
constexpr static TickType_t INPUT_STARTUP_STABILIZE_MS = 2000;
constexpr static TickType_t INPUT_STUCK_LOW_MS = 1000;
constexpr static TickType_t INPUT_BOUNCE_WINDOW_MS = 5000;

static inline void set_progress_pixel(LED_Strip *strip, uint16_t index, RUNE_COLOR color, uint8_t brightness)
{
    strip->set_color_index(index, color == PR_RED ? brightness : 0, 0, color == PR_RED ? 0 : brightness);
}

// 大能量机关特殊等效：MATRIX进度条从底部(index 164)按1/5累计点亮
static inline void apply_big_rune_matrix_progress(LED_Strip *matrix_strip, uint8_t progress_stage, RUNE_COLOR color, uint8_t brightness)
{
    if (progress_stage == 0)
        return;
    uint16_t leds_to_light = (MATRIX_LED_COUNT * progress_stage + BIG_RUNE_PROGRESS_STAGE_TOTAL - 1) / BIG_RUNE_PROGRESS_STAGE_TOTAL;
    if (leds_to_light > MATRIX_LED_COUNT)
        leds_to_light = MATRIX_LED_COUNT;
    for (uint16_t i = 0; i < leds_to_light; i++)
    {
        set_progress_pixel(matrix_strip, MATRIX_LED_COUNT - 1 - i, color, brightness);
    }
}

// 大能量机关特殊等效：ARM进度条以底边10灯为基准向两侧/上方按1/5累计点亮
static inline void apply_big_rune_arm_progress(LED_Strip *arm_strip, uint8_t progress_stage, RUNE_COLOR color, uint8_t brightness)
{
    if (progress_stage == 0)
        return;
    uint16_t leds_to_light = (ARM_LED_COUNT * progress_stage + BIG_RUNE_PROGRESS_STAGE_TOTAL - 1) / BIG_RUNE_PROGRESS_STAGE_TOTAL;
    if (leds_to_light > ARM_LED_COUNT)
        leds_to_light = ARM_LED_COUNT;

    uint16_t lit_count = 0;
    const uint16_t bottom_start = ARM_BOTTOM_START_INDEX;
    const uint16_t bottom_end = ARM_BOTTOM_START_INDEX + ARM_BOTTOM_LED_COUNT - 1;

    for (uint16_t idx = bottom_start; idx <= bottom_end && lit_count < leds_to_light; idx++)
    {
        set_progress_pixel(arm_strip, idx, color, brightness);
        lit_count++;
    }

    for (uint16_t offset = 1; lit_count < leds_to_light; offset++)
    {
        bool wrote = false;
        if (bottom_start >= offset)
        {
            set_progress_pixel(arm_strip, bottom_start - offset, color, brightness);
            lit_count++;
            wrote = true;
            if (lit_count >= leds_to_light)
                break;
        }
        uint16_t right_index = bottom_end + offset;
        if (right_index < ARM_LED_COUNT)
        {
            set_progress_pixel(arm_strip, right_index, color, brightness);
            lit_count++;
            wrote = true;
        }
        if (!wrote)
            break;
    }
}

void PowerRune_Armour::clear_armour(bool refresh)
{
    for (uint8_t i = 0; i < 5; i++)
    {
        demux_led = i;
        led_strip[i]->clear_pixels();
        if (refresh)
            led_strip[i]->refresh();
    }
}

// LED更新任务
void PowerRune_Armour::LED_update_task(void *pvParameter)
{
    // 状态
    LED_Strip_FSM_t state_task;
    const PowerRune_Armour_config_info_t *config_info;

    // 清除所有灯效
    for (uint8_t i = 0; i < 5; i++)
    {
        demux_led = i;
        led_strip[i]->refresh();
    }
    while (1)
    {
        // 状态机，状态转移：START->IDLE->TARGET->HIT->BLINK->IDLE，TARGET->HIT和BLINK->IDLE过程之后task阻塞，等待信号量
        switch (state.LED_Strip_State)
        {
        case LED_STRIP_DEBUG:
        {
            do
            { // 初始化状态，使用valid变量显示损坏的装甲板
                demux_led = LED_STRIP_MAIN_ARMOUR;
                // led_strip[LED_STRIP_MAIN_ARMOUR]->clear_pixels();
                for (size_t i = 0; i < 10; i++)
                {
                    if (!valid[i])
                        for (uint16_t j = hit_ring_cutoff[i]; j < hit_ring_cutoff[i + 1]; j++)
                            led_strip[LED_STRIP_MAIN_ARMOUR]->set_color_index(j, 0, 100, 0); // Green
                    led_strip[LED_STRIP_MAIN_ARMOUR]->refresh();
                }
            } while (xSemaphoreTake(LED_Strip_FSM_Semaphore, 0) == pdFALSE);
            // 转移状态
            state_task = state;
            break;
        }
        case LED_STRIP_IDLE:
            clear_armour();
            // 等待信号量
            xSemaphoreTake(LED_Strip_FSM_Semaphore, portMAX_DELAY);
            // 转移状态
            state_task = state;
            break;
        case LED_STRIP_TARGET:
        {
            clear_armour(false);
            config_info = config->get_config_info_pt();

            // 点亮靶状图案、上下装甲板，灯臂刷新一次
            demux_led = LED_STRIP_MAIN_ARMOUR;
            const bool is_big_mode_like =
                (state_task.mode == PRA_RUNE_BIG_MODE || state_task.mode == PRA_RUNE_BIG_PROGRESS_2_READY_TEST_MODE);
            const bool is_small_mode = (state_task.mode == PRA_RUNE_SMALL_MODE);
            const bool target_armed = (big_rune_target_armed != 0);
            const bool show_small_target_aux =
                target_armed &&
                (is_small_mode ||
                 state_task.mode == PRA_RUNE_SINGLE_TEST_MODE ||
                 state_task.mode == PRA_RUNE_SINGLE_SCORE_TEST_MODE ||
                 state_task.mode == PRA_RUNE_SMALL_4_HIT_1_READY_TEST_MODE ||
                 state_task.mode == PRA_RUNE_ALL_TARGET_READY_MODE);
            bool draw_target_pic = false;
            if (((is_small_mode ||
                  state_task.mode == PRA_RUNE_SINGLE_TEST_MODE ||
                  state_task.mode == PRA_RUNE_SINGLE_SCORE_TEST_MODE) &&
                 target_armed) ||
                state_task.mode == PRA_RUNE_ALL_TARGET_READY_MODE ||
                state_task.mode == PRA_RUNE_SMALL_4_HIT_1_READY_TEST_MODE)
            {
                draw_target_pic = true;
            }
            else if (is_big_mode_like && target_armed)
            {
                // 大能量机关特殊等效：仅待击打叶片显示靶面target_pic
                draw_target_pic = true;
            }
            if (draw_target_pic)
            {
                if (state_task.color == PR_RED)
                {
                    for (uint16_t i = 0; i < sizeof(target_pic) / sizeof(uint16_t); i++)
                    {
                        led_strip[LED_STRIP_MAIN_ARMOUR]->set_color_index(target_pic[i], config_info->brightness, 0, 0);
                    }
                }
                else
                {
                    for (uint16_t i = 0; i < sizeof(target_pic) / sizeof(uint16_t); i++)
                    {
                        led_strip[LED_STRIP_MAIN_ARMOUR]->set_color_index(target_pic[i], 0, 0, config_info->brightness);
                    }
                }
            }
            else
            {
                // 大能量机关特殊等效：非待击打叶片不显示靶面，仅保留进度条
                led_strip[LED_STRIP_MAIN_ARMOUR]->clear_pixels();
            }
            led_strip[LED_STRIP_MAIN_ARMOUR]->refresh();
            if (show_small_target_aux)
            {
                demux_led = LED_STRIP_UPPER;
                led_strip[LED_STRIP_UPPER]->set_color(state_task.color == PR_RED ? config_info->brightness : 0, 0, state_task.color == PR_RED ? 0 : config_info->brightness);
                led_strip[LED_STRIP_UPPER]->refresh();
                demux_led = LED_STRIP_LOWER;
                led_strip[LED_STRIP_LOWER]->set_color(state_task.color == PR_RED ? config_info->brightness : 0, 0, state_task.color == PR_RED ? 0 : config_info->brightness);
                led_strip[LED_STRIP_LOWER]->refresh();
            }
            else
            {
                demux_led = LED_STRIP_UPPER;
                led_strip[LED_STRIP_UPPER]->clear_pixels();
                led_strip[LED_STRIP_UPPER]->refresh();
                demux_led = LED_STRIP_LOWER;
                led_strip[LED_STRIP_LOWER]->clear_pixels();
                led_strip[LED_STRIP_LOWER]->refresh();
            }
            demux_led = LED_STRIP_ARM;
            led_strip[LED_STRIP_ARM]->clear_pixels();
            if (is_big_mode_like)
            {
                if (target_armed)
                {
                    // 大能量机关特殊等效：待击打叶片ARM保持熄灭，仅用MATRIX流水提示
                }
                else
                {
                    // 大能量机关特殊等效：非待击打叶片显示进度条
                    apply_big_rune_arm_progress(led_strip[LED_STRIP_ARM], big_rune_progress_stage, state_task.color, config_info->brightness_proportion_edge);
                }
            }
            else if (show_small_target_aux)
            {
                led_strip[LED_STRIP_ARM]->set_color(state_task.color == PR_RED ? config_info->brightness_proportion_edge : 0, 0, state_task.color == PR_RED ? 0 : config_info->brightness_proportion_edge);
            }
            led_strip[LED_STRIP_ARM]->refresh();
            // 开启矩阵流水灯
            uint8_t i = 0;
            do
            {
                TickType_t xLastWakeTime = xTaskGetTickCount();

                demux_led = LED_STRIP_MATRIX;

                if (is_big_mode_like)
                {
                    if (target_armed)
                    {
                        // 大能量机关特殊等效：待击打叶片显示流水等效
                        if (state_task.color == PR_RED)
                            for (uint16_t j = 0; j < 165; j++)
                            {
                                led_strip[LED_STRIP_MATRIX]->set_color_index(j, single_arrow[(j + i * 5) % 25] * config_info->brightness, 0, 0);
                            }
                        else
                            for (uint16_t j = 0; j < 165; j++)
                            {
                                led_strip[LED_STRIP_MATRIX]->set_color_index(j, 0, 0, single_arrow[(j + i * 5) % 25] * config_info->brightness);
                            }
                    }
                    else
                    {
                        // 大能量机关特殊等效：非待击打叶片关闭流水，仅显示进度条
                        led_strip[LED_STRIP_MATRIX]->clear_pixels();
                        apply_big_rune_matrix_progress(led_strip[LED_STRIP_MATRIX], big_rune_progress_stage, state_task.color, config_info->brightness_proportion_matrix);
                    }
                }
                else if (is_small_mode && !target_armed)
                {
                    // 小符正式模式非待击打叶片保持熄灭，但仍允许GPIO命中上报给主控判错
                    led_strip[LED_STRIP_MATRIX]->clear_pixels();
                }
                else
                {
                    if (state_task.color == PR_RED)
                        for (uint16_t j = 0; j < 165; j++)
                        {
                            led_strip[LED_STRIP_MATRIX]->set_color_index(j, single_arrow[(j + i * 5) % 25] * config_info->brightness, 0, 0);
                        }
                    else
                        for (uint16_t j = 0; j < 165; j++)
                        {
                            led_strip[LED_STRIP_MATRIX]->set_color_index(j, 0, 0, single_arrow[(j + i * 5) % 25] * config_info->brightness);
                        }
                }
                led_strip[LED_STRIP_MATRIX]->refresh();
                i = (i + 1) % 5;
                vTaskDelayUntil(&xLastWakeTime, MATRIX_REFRESH_PERIOD / portTICK_PERIOD_MS);

            } while (xSemaphoreTake(LED_Strip_FSM_Semaphore, 0) == pdFALSE);
            // 信号量释放后，重新加载state_task
            state_task = state;
            break;
        }
        case LED_STRIP_SUCCESS_STATIC:
        {
            clear_armour(false);
            config_info = config->get_config_info_pt();

            demux_led = LED_STRIP_MAIN_ARMOUR;
            led_strip[LED_STRIP_MAIN_ARMOUR]->clear_pixels();
            for (uint16_t j = hit_ring_cutoff[7]; j < hit_ring_cutoff[8]; j++)
            {
                led_strip[LED_STRIP_MAIN_ARMOUR]->set_color_index(j, state_task.color == PR_RED ? config_info->brightness : 0, 0, state_task.color == PR_RED ? 0 : config_info->brightness);
            }
            led_strip[LED_STRIP_MAIN_ARMOUR]->refresh();

            demux_led = LED_STRIP_UPPER;
            led_strip[LED_STRIP_UPPER]->set_color(state_task.color == PR_RED ? config_info->brightness : 0, 0, state_task.color == PR_RED ? 0 : config_info->brightness);
            led_strip[LED_STRIP_UPPER]->refresh();

            demux_led = LED_STRIP_LOWER;
            led_strip[LED_STRIP_LOWER]->set_color(state_task.color == PR_RED ? config_info->brightness : 0, 0, state_task.color == PR_RED ? 0 : config_info->brightness);
            led_strip[LED_STRIP_LOWER]->refresh();

            demux_led = LED_STRIP_ARM;
            led_strip[LED_STRIP_ARM]->set_color(state_task.color == PR_RED ? config_info->brightness_proportion_edge : 0, 0, state_task.color == PR_RED ? 0 : config_info->brightness_proportion_edge);
            led_strip[LED_STRIP_ARM]->refresh();

            demux_led = LED_STRIP_MATRIX;
            led_strip[LED_STRIP_MATRIX]->set_color(state_task.color == PR_RED ? config_info->brightness_proportion_matrix : 0, 0, state_task.color == PR_RED ? 0 : config_info->brightness_proportion_matrix);
            led_strip[LED_STRIP_MATRIX]->refresh();

            xSemaphoreTake(LED_Strip_FSM_Semaphore, portMAX_DELAY);
            state_task = state;
            break;
        }
        case LED_STRIP_SMALL_HIT_STATIC:
        {
            clear_armour(false);
            config_info = config->get_config_info_pt();

            demux_led = LED_STRIP_UPPER;
            led_strip[LED_STRIP_UPPER]->clear_pixels();
            led_strip[LED_STRIP_UPPER]->refresh();

            demux_led = LED_STRIP_LOWER;
            led_strip[LED_STRIP_LOWER]->clear_pixels();
            led_strip[LED_STRIP_LOWER]->refresh();

            demux_led = LED_STRIP_MAIN_ARMOUR;
            led_strip[LED_STRIP_MAIN_ARMOUR]->clear_pixels();
            for (uint16_t j = hit_ring_cutoff[0]; j < hit_ring_cutoff[1]; j++)
            {
                led_strip[LED_STRIP_MAIN_ARMOUR]->set_color_index(j, state_task.color == PR_RED ? config_info->brightness : 0, 0, state_task.color == PR_RED ? 0 : config_info->brightness);
            }
            led_strip[LED_STRIP_MAIN_ARMOUR]->refresh();

            demux_led = LED_STRIP_ARM;
            led_strip[LED_STRIP_ARM]->set_color(state_task.color == PR_RED ? config_info->brightness_proportion_edge : 0, 0, state_task.color == PR_RED ? 0 : config_info->brightness_proportion_edge);
            led_strip[LED_STRIP_ARM]->refresh();

            demux_led = LED_STRIP_MATRIX;
            led_strip[LED_STRIP_MATRIX]->set_color(state_task.color == PR_RED ? config_info->brightness_proportion_matrix : 0, 0, state_task.color == PR_RED ? 0 : config_info->brightness_proportion_matrix);
            led_strip[LED_STRIP_MATRIX]->refresh();

            xSemaphoreTake(LED_Strip_FSM_Semaphore, portMAX_DELAY);
            state_task = state;
            break;
        }
        case LED_STRIP_HIT:
        {
            config_info = config->get_config_info_pt();
            // 命中图案，大符为对应环数（10环特殊，点亮），小符为1环
            switch (state_task.mode)
            {
            case PRA_RUNE_BIG_MODE:
                demux_led = LED_STRIP_MAIN_ARMOUR;
                led_strip[LED_STRIP_MAIN_ARMOUR]->clear_pixels();
                for (uint16_t j = hit_ring_cutoff[state_task.score - 1]; j < hit_ring_cutoff[state_task.score]; j++)
                    led_strip[LED_STRIP_MAIN_ARMOUR]->set_color_index(j, state_task.color == PR_RED ? config_info->brightness : 0, 0, state_task.color == PR_RED ? 0 : config_info->brightness);

                if (state_task.score == 10)
                {
                    // 点亮1，3，5，7，9环
                    for (uint16_t i = 0; i < 5; i++)
                        for (uint16_t j = hit_ring_cutoff[i * 2]; j < hit_ring_cutoff[i * 2 + 1]; j++)
                            led_strip[LED_STRIP_MAIN_ARMOUR]->set_color_index(j, state_task.color == PR_RED ? config_info->brightness : 0, 0, state_task.color == PR_RED ? 0 : config_info->brightness);
                }
                led_strip[LED_STRIP_MAIN_ARMOUR]->refresh();
                demux_led = LED_STRIP_ARM;
                led_strip[LED_STRIP_ARM]->set_color(state_task.color == PR_RED ? config_info->brightness_proportion_edge : 0, 0, state_task.color == PR_RED ? 0 : config_info->brightness_proportion_edge);
                led_strip[LED_STRIP_ARM]->refresh();
                demux_led = LED_STRIP_MATRIX;
                led_strip[LED_STRIP_MATRIX]->set_color(state_task.color == PR_RED ? config_info->brightness_proportion_matrix : 0, 0, state_task.color == PR_RED ? 0 : config_info->brightness_proportion_matrix);
                led_strip[LED_STRIP_MATRIX]->refresh();
                // 命中后的完成事件需要等命中灯效显示后再切换到BLINK
                if (!pending_blink_after_hit)
                    xSemaphoreTake(LED_Strip_FSM_Semaphore, portMAX_DELAY);
                if (pending_blink_after_hit)
                {
                    state.LED_Strip_State = LED_STRIP_BLINK;
                    pending_blink_after_hit = false;
                }
                // 转移状态
                state_task = state;
                break;
            case PRA_RUNE_SINGLE_TEST_MODE:
            case PRA_RUNE_SMALL_MODE:
            case PRA_RUNE_SMALL_4_HIT_1_READY_TEST_MODE:
                demux_led = LED_STRIP_UPPER;
                led_strip[LED_STRIP_UPPER]->clear_pixels();
                led_strip[LED_STRIP_UPPER]->refresh();
                demux_led = LED_STRIP_LOWER;
                led_strip[LED_STRIP_LOWER]->clear_pixels();
                led_strip[LED_STRIP_LOWER]->refresh();
                demux_led = LED_STRIP_MAIN_ARMOUR;
                led_strip[LED_STRIP_MAIN_ARMOUR]->clear_pixels();
                for (uint16_t j = hit_ring_cutoff[0]; j < hit_ring_cutoff[1]; j++)
                {
                    led_strip[LED_STRIP_MAIN_ARMOUR]->set_color_index(j, state_task.color == PR_RED ? config_info->brightness : 0, 0, state_task.color == PR_RED ? 0 : config_info->brightness);
                }
                led_strip[LED_STRIP_MAIN_ARMOUR]->refresh();
                demux_led = LED_STRIP_ARM;
                led_strip[LED_STRIP_ARM]->set_color(state_task.color == PR_RED ? config_info->brightness_proportion_edge : 0, 0, state_task.color == PR_RED ? 0 : config_info->brightness_proportion_edge);
                led_strip[LED_STRIP_ARM]->refresh();
                demux_led = LED_STRIP_MATRIX;
                led_strip[LED_STRIP_MATRIX]->set_color(state_task.color == PR_RED ? config_info->brightness_proportion_matrix : 0, 0, state_task.color == PR_RED ? 0 : config_info->brightness_proportion_matrix);
                led_strip[LED_STRIP_MATRIX]->refresh();
                // 命中后的完成事件需要等命中灯效显示后再切换到BLINK
                if (!pending_blink_after_hit)
                    xSemaphoreTake(LED_Strip_FSM_Semaphore, portMAX_DELAY);
                if (pending_blink_after_hit)
                {
                    state.LED_Strip_State = LED_STRIP_BLINK;
                    pending_blink_after_hit = false;
                }
                // 转移状态
                state_task = state;
                break;
            case PRA_RUNE_SMALL_HIT_STATIC_MODE:
                state.LED_Strip_State = LED_STRIP_SMALL_HIT_STATIC;
                state_task = state;
                break;
            case PRA_RUNE_ALL_TARGET_READY_MODE:
            case PRA_RUNE_ALL_SUCCESS_STATIC_MODE:
                state.LED_Strip_State = (state_task.mode == PRA_RUNE_ALL_SUCCESS_STATIC_MODE) ? LED_STRIP_SUCCESS_STATIC : LED_STRIP_TARGET;
                state_task = state;
                break;
            default:
                state.LED_Strip_State = LED_STRIP_IDLE;
                state_task = state;
                break;
            }
            break;
        }
        case LED_STRIP_BLINK:
        {

            config_info = config->get_config_info_pt();
            // 按ID进行同步化延迟
            vTaskDelay((BLINK_DELAY * (6 - config_info->armour_id)) / portTICK_PERIOD_MS);
            // 成功等效：主甲板固定显示第8环（小符/大符）
            if (state_task.mode == PRA_RUNE_SMALL_MODE || state_task.mode == PRA_RUNE_BIG_MODE)
            {
                demux_led = LED_STRIP_MAIN_ARMOUR;
                led_strip[LED_STRIP_MAIN_ARMOUR]->clear_pixels();
                for (uint16_t j = hit_ring_cutoff[7]; j < hit_ring_cutoff[8]; j++)
                {
                    led_strip[LED_STRIP_MAIN_ARMOUR]->set_color_index(j, state_task.color == PR_RED ? config_info->brightness : 0, 0, state_task.color == PR_RED ? 0 : config_info->brightness);
                }
                led_strip[LED_STRIP_MAIN_ARMOUR]->refresh();
            }
            // UPPER，LOWER，MATRIX，ARM闪烁十次，MAIN_ARMOUR不闪烁
            for (uint8_t i = 0; i < 10; i++)
            {
                demux_led = LED_STRIP_UPPER;
                led_strip[LED_STRIP_UPPER]->clear_pixels();
                led_strip[LED_STRIP_UPPER]->refresh();
                demux_led = LED_STRIP_LOWER;
                led_strip[LED_STRIP_LOWER]->clear_pixels();
                led_strip[LED_STRIP_LOWER]->refresh();
                demux_led = LED_STRIP_MATRIX;
                led_strip[LED_STRIP_MATRIX]->clear_pixels();
                led_strip[LED_STRIP_MATRIX]->refresh();
                demux_led = LED_STRIP_ARM;
                led_strip[LED_STRIP_ARM]->clear_pixels();
                led_strip[LED_STRIP_ARM]->refresh();
                vTaskDelay(100 / portTICK_PERIOD_MS);
                demux_led = LED_STRIP_UPPER;
                led_strip[LED_STRIP_UPPER]->set_color(state_task.color == PR_RED ? config_info->brightness : 0, 0, state_task.color == PR_RED ? 0 : config_info->brightness);
                led_strip[LED_STRIP_UPPER]->refresh();
                demux_led = LED_STRIP_LOWER;
                led_strip[LED_STRIP_LOWER]->set_color(state_task.color == PR_RED ? config_info->brightness : 0, 0, state_task.color == PR_RED ? 0 : config_info->brightness);
                led_strip[LED_STRIP_LOWER]->refresh();
                demux_led = LED_STRIP_MATRIX;
                led_strip[LED_STRIP_MATRIX]->set_color(state_task.color == PR_RED ? config_info->brightness_proportion_matrix : 0, 0, state_task.color == PR_RED ? 0 : config_info->brightness_proportion_matrix);
                led_strip[LED_STRIP_MATRIX]->refresh();
                demux_led = LED_STRIP_ARM;
                led_strip[LED_STRIP_ARM]->set_color(state_task.color == PR_RED ? config_info->brightness_proportion_edge : 0, 0, state_task.color == PR_RED ? 0 : config_info->brightness_proportion_edge);
                led_strip[LED_STRIP_ARM]->refresh();
                vTaskDelay(100 / portTICK_PERIOD_MS);
            }
            // 等待信号量
            xSemaphoreTake(LED_Strip_FSM_Semaphore, portMAX_DELAY);
            // 转移状态
            state_task = state;
            break;
        }
        }
    }
    vTaskDelete(NULL);
}

void IRAM_ATTR PowerRune_Armour::GPIO_ISR_handler(void *arg)
{
    // 操作过程中激活互斥锁，屏蔽其他中断
    if (xSemaphoreTake(ISR_mutex, 0) == pdFALSE)
        return;
    uint8_t io = (*(uint8_t *)arg);
    // 发送事件
    PRA_HIT_EVENT_DATA hit_event_data;
    hit_event_data.address = config->get_config_info_pt()->armour_id - 1;
    hit_event_data.score = io;
    esp_event_post_to(pr_events_loop_handle, PRA, PRA_HIT_EVENT, &hit_event_data, sizeof(PRA_HIT_EVENT_DATA), portMAX_DELAY);
    xTaskCreate(restart_ISR_task, "restart_ISR_task", 4096, NULL, 5, NULL);
}

void PowerRune_Armour::GPIO_init()
{
    // 初始化GPIO
    gpio_config_t io_conf;
    // io_conf.intr_type = GPIO_INTR_NEGEDGE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    for (uint8_t i = 0; i < 10; i++)
    {
        io_conf.pin_bit_mask = (1ULL << TRIGGER_IO[i]);
        gpio_config(&io_conf);
    }
}

void PowerRune_Armour::GPIO_polling_service(void *pvParameter)
{
    uint8_t io_valid_state[10] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    uint8_t io_last_valid_state[10] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    uint8_t io_last_reading[10] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    TickType_t last_jump_time[10] = {0};
    TickType_t bounce_time[10] = {0};
    TickType_t activation_time[10] = {0};
    TickType_t last_activation_time = 0;
    uint32_t bounce_count[10] = {0}; // 消抖计数

    ESP_LOGI(TAG_ARMOUR, "GPIO polling service start");
    TickType_t start_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
    // GPIO命中上报门控：仅允许当前轮次的目标态叶片上报PRA_HIT_EVENT，是否判有效由主控决定
    auto allow_gpio_hit_report = [&](uint8_t score) -> bool {
        if (state.LED_Strip_State != LED_STRIP_TARGET)
        {
            ESP_LOGW(TAG_ARMOUR, "Drop GPIO hit score=%u: state=%d mode=%d armed=%d",
                     score, (int)state.LED_Strip_State, (int)state.mode, (int)big_rune_target_armed);
            return false;
        }

        switch (state.mode)
        {
        case PRA_RUNE_SMALL_MODE:
        case PRA_RUNE_SINGLE_TEST_MODE:
            return true;
        case PRA_RUNE_SMALL_4_HIT_1_READY_TEST_MODE:
        case PRA_RUNE_SMALL_HIT_STATIC_MODE:
        case PRA_RUNE_BIG_PROGRESS_2_READY_TEST_MODE:
            ESP_LOGW(TAG_ARMOUR, "Drop GPIO hit score=%u: state=%d mode=%d armed=%d",
                     score, (int)state.LED_Strip_State, (int)state.mode, (int)big_rune_target_armed);
            return false;
        case PRA_RUNE_BIG_MODE:
            return true;
        case PRA_RUNE_ALL_TARGET_READY_MODE:
        case PRA_RUNE_ALL_SUCCESS_STATIC_MODE:
        default:
            ESP_LOGW(TAG_ARMOUR, "Drop GPIO hit score=%u: state=%d mode=%d armed=%d",
                     score, (int)state.LED_Strip_State, (int)state.mode, (int)big_rune_target_armed);
            return false;
        }
    };

    while (1)
    {
        TickType_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
        const bool startup_stabilized = (current_time - start_time) > INPUT_STARTUP_STABILIZE_MS;
        for (uint8_t i = 0; i < 10; i++)
        {
            int reading = gpio_get_level(TRIGGER_IO[i]);

            // 去抖动处理
            if (reading != io_last_reading[i] && valid[i])
            {
                last_jump_time[i] = current_time;
                bounce_count[i]++;
                ESP_LOGI(TAG_ARMOUR, "GPIO %d, Bounce Count: %d", (int)TRIGGER_IO[i], (int)bounce_count[i]);
                io_last_reading[i] = reading;
            }

            if ((current_time - last_jump_time[i]) > 1) // 消抖时间
            {
                if (valid[i] && (reading != io_valid_state[i])) // 有效触发
                {
                    if (reading == 0 && current_time - last_activation_time > INPUT_STUCK_LOW_MS) // LOW level，触发间隔需大于1s
                    {
                        io_valid_state[i] = reading;
                        last_activation_time = current_time;
                        activation_time[i] = current_time;
                        ESP_LOGI(TAG_ARMOUR, "GPIO %d, Score IO %d Triggered", (int)TRIGGER_IO[i], (int)TRIGGER_IO_TO_SCORE[i]);
                    }
                    else if (reading == 1)
                    {
                        io_valid_state[i] = reading;
                        ESP_LOGI(TAG_ARMOUR, "GPIO %d, Score IO %d Released", (int)TRIGGER_IO[i], (int)TRIGGER_IO_TO_SCORE[i]);
                    }
                }
            }

            // 持续低电平激活检测
            if (startup_stabilized && io_valid_state[i] == 0 && current_time - activation_time[i] > INPUT_STUCK_LOW_MS && valid[i])
            {
                valid[i] = false;
                ESP_LOGW(TAG_ARMOUR, "GPIO %d (score ring %d) damaged: Low level detected for too long after startup stabilization.",
                         (int)TRIGGER_IO[i], (int)TRIGGER_IO_TO_SCORE[i]);
                io_valid_state[i] = 1;
            }

            // 跳变检测，防止键轴卡阻
            if (startup_stabilized && (current_time - bounce_time[i]) < INPUT_BOUNCE_WINDOW_MS && valid[i])
            {
                if (bounce_count[i] > 10) // 5秒内跳变次数超过15次，认为是键轴卡阻
                {
                    valid[i] = false;
                    ESP_LOGW(TAG_ARMOUR, "GPIO %d (score ring %d) damaged: Frequent bouncing detected after startup stabilization.",
                             (int)TRIGGER_IO[i], (int)TRIGGER_IO_TO_SCORE[i]);
                    io_valid_state[i] = 1;
                    bounce_count[i] = 0;
                    bounce_time[i] = current_time;
                }
            }
            else if (valid[i])
            {
                bounce_count[i] = 0;
                bounce_time[i] = current_time;
            }
            else if (!valid[i])
            {
                bounce_count[i] = 0;
            }

            if (startup_stabilized) // 预留2s供按键检测
            {
                if (io_valid_state[i] == 0 && valid[i] && io_last_valid_state[i] == 1) // 下升沿触发，保证实时性
                {
                    const uint8_t score = TRIGGER_IO_TO_SCORE[i];
                    if (allow_gpio_hit_report(score))
                    {
                        ESP_LOGI(TAG_ARMOUR, "GPIO %d, Score IO %d Sending event...", TRIGGER_IO[i], score);
                        // 发送事件
                        PRA_HIT_EVENT_DATA hit_event_data;
                        hit_event_data.address = config->get_config_info_pt()->armour_id - 1;
                        hit_event_data.score = score;
                        esp_event_post_to(pr_events_loop_handle, PRA, PRA_HIT_EVENT, &hit_event_data, sizeof(PRA_HIT_EVENT_DATA), portMAX_DELAY);
                    }
                    io_last_valid_state[i] = io_valid_state[i];
                }
                else if (io_valid_state[i] == 1 && valid[i] && io_last_valid_state[i] == 0)
                {
                    io_last_valid_state[i] = io_valid_state[i];
                }
            }
        }
        vTaskDelay(1 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

void PowerRune_Armour::GPIO_ISR_enable()
{
    // 初始化GPIO ISR
    for (uint8_t i = 0; i < 10; i++)
    {
        gpio_set_intr_type(TRIGGER_IO[i], GPIO_INTR_NEGEDGE);
    }
}

void PowerRune_Armour::restart_ISR_task(void *pvParameter)
{
    // 屏蔽1s
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    // 释放信号量
    xSemaphoreGive(ISR_mutex);
    vTaskDelete(NULL);
}

// Class PowerRune_Armour 定义
PowerRune_Armour::PowerRune_Armour()
{
    // 初始化GPIO
    GPIO_init();
    // 开启ISR服务
    // gpio_install_isr_service(0);
    // for (uint8_t i = 0; i < 10; i++)
    // {
    // gpio_isr_handler_add(TRIGGER_IO[i], GPIO_ISR_handler, (void *)&TRIGGER_IO_TO_SCORE[i]);
    // }

    // GPIO_ISR_enable();
    // 开启GPIO轮询服务
    xTaskCreate(GPIO_polling_service, "GPIO_polling_service", 4096, NULL, 5, NULL);
    // 初始化LED_Strip
    led_strip[LED_STRIP_MAIN_ARMOUR] = new LED_Strip(STRIP_IO, 272);
    led_strip[LED_STRIP_UPPER] = new LED_Strip(STRIP_IO, 86);
    led_strip[LED_STRIP_LOWER] = new LED_Strip(STRIP_IO, 92);
    led_strip[LED_STRIP_ARM] = new LED_Strip(STRIP_IO, 82);
    led_strip[LED_STRIP_MATRIX] = new LED_Strip(STRIP_IO, 165);

    // 状态机更新信号量
    LED_Strip_FSM_Semaphore = xSemaphoreCreateBinary();
    xSemaphoreGive(ISR_mutex);
    // 创建LED更新任务
    xTaskCreate(LED_update_task, "LED_update_task", 8192, NULL, 5, &LED_update_task_handle);

    // 注册装甲板事件处理
    ESP_ERROR_CHECK(esp_event_handler_register_with(pr_events_loop_handle, PRA, PRA_START_EVENT, global_pr_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register_with(pr_events_loop_handle, PRA, PRA_HIT_EVENT, global_pr_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register_with(pr_events_loop_handle, PRA, PRA_STOP_EVENT, global_pr_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register_with(pr_events_loop_handle, PRA, PRA_COMPLETE_EVENT, global_pr_event_handler, NULL));
    // OTA事件处理
    ESP_ERROR_CHECK(esp_event_handler_register_with(pr_events_loop_handle, PRC, OTA_BEGIN_EVENT, global_pr_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register_with(pr_events_loop_handle, PRC, OTA_COMPLETE_EVENT, global_pr_event_handler, NULL));
}

void PowerRune_Armour::trigger(RUNE_MODE mode, RUNE_COLOR color, uint8_t group_index, uint8_t target_armed)
{
    const char *mode_text = "Big";
    switch (mode)
    {
    case PRA_RUNE_SMALL_MODE:
    case PRA_RUNE_SINGLE_TEST_MODE:
        mode_text = "Small";
        break;
    case PRA_RUNE_ALL_TARGET_READY_MODE:
        mode_text = "AllTargetReady";
        break;
    case PRA_RUNE_ALL_SUCCESS_STATIC_MODE:
        mode_text = "AllSuccessStatic";
        break;
    case PRA_RUNE_SMALL_4_HIT_1_READY_TEST_MODE:
        mode_text = "Small4Hit1ReadyTest";
        break;
    case PRA_RUNE_SMALL_HIT_STATIC_MODE:
        mode_text = "SmallHitStatic";
        break;
    case PRA_RUNE_BIG_PROGRESS_2_READY_TEST_MODE:
        mode_text = "BigProgress2ReadyTest";
        break;
    default:
        break;
    }
    ESP_LOGI(TAG_ARMOUR, "Trigger Armour with mode: %s, color: %s", mode_text, color == PR_RED ? "Red" : "Blue");
    pending_blink_after_hit = false;
    if (mode == PRA_RUNE_BIG_MODE || mode == PRA_RUNE_BIG_PROGRESS_2_READY_TEST_MODE)
    {
        if (group_index < 1 || group_index > BIG_RUNE_PROGRESS_STAGE_TOTAL)
            group_index = 1;
        big_rune_group_index = group_index;
        big_rune_target_armed = target_armed ? 1 : 0;
        big_rune_progress_stage = big_rune_group_index - 1;
        if (big_rune_progress_stage >= BIG_RUNE_PROGRESS_STAGE_TOTAL)
            big_rune_progress_stage = BIG_RUNE_PROGRESS_STAGE_TOTAL - 1;
    }
    else
    {
        big_rune_group_index = 1;
        big_rune_target_armed = (mode == PRA_RUNE_SMALL_MODE) ? (target_armed ? 1 : 0) : 1;
        big_rune_progress_stage = 0;
    }
    if (mode == PRA_RUNE_BIG_MODE || mode == PRA_RUNE_BIG_PROGRESS_2_READY_TEST_MODE)
    {
        ESP_LOGI(TAG_ARMOUR, "[BigRune Sync] state=%d group=%d armed=%d stage=%d/%d",
                 (int)state.LED_Strip_State, (int)big_rune_group_index,
                 (int)big_rune_target_armed, (int)big_rune_progress_stage,
                 (int)BIG_RUNE_PROGRESS_STAGE_TOTAL);
    }
    // 状态机更新
    if (mode == PRA_RUNE_ALL_SUCCESS_STATIC_MODE)
        state.LED_Strip_State = LED_STRIP_SUCCESS_STATIC;
    else if (mode == PRA_RUNE_SMALL_HIT_STATIC_MODE)
        state.LED_Strip_State = LED_STRIP_SMALL_HIT_STATIC;
    else
        state.LED_Strip_State = LED_STRIP_TARGET;
    state.mode = mode;
    state.color = color;
    state.score = 0;
    // 释放信号量
    xSemaphoreGive(LED_Strip_FSM_Semaphore);
}

void PowerRune_Armour::stop()
{
    ESP_LOGI(TAG_ARMOUR, "Stop Armour");
    pending_blink_after_hit = false;
    big_rune_group_index = 1;
    big_rune_target_armed = 1;
    big_rune_progress_stage = 0;
    // 状态机更新
    state.LED_Strip_State = LED_STRIP_IDLE;
    // 释放信号量
    xSemaphoreGive(LED_Strip_FSM_Semaphore);
}

void PowerRune_Armour::debug()
{
    ESP_LOGI(TAG_ARMOUR, "Debug Armour");
    pending_blink_after_hit = false;
    big_rune_group_index = 1;
    big_rune_target_armed = 1;
    big_rune_progress_stage = 0;
    // 状态机更新
    state.LED_Strip_State = LED_STRIP_DEBUG;
    // 释放信号量
    xSemaphoreGive(LED_Strip_FSM_Semaphore);
}

void PowerRune_Armour::hit(uint8_t score)
{
    ESP_LOGI(TAG_ARMOUR, "Hit Armour with score: %d", score);
    // 状态机更新
    state.LED_Strip_State = LED_STRIP_HIT;
    state.score = score;
    // 释放信号量
    xSemaphoreGive(LED_Strip_FSM_Semaphore);
}

void PowerRune_Armour::blink()
{
    ESP_LOGI(TAG_ARMOUR, "Activation Complete, Blink Armour");
    pending_blink_after_hit = false;
    big_rune_group_index = 1;
    big_rune_target_armed = 1;
    big_rune_progress_stage = 0;
    // 状态机更新
    state.LED_Strip_State = LED_STRIP_BLINK;
    // 释放信号量
    xSemaphoreGive(LED_Strip_FSM_Semaphore);
}

void PowerRune_Armour::global_pr_event_handler(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    if (base == PRA)
        switch (id)
        {
        case PRA_START_EVENT:
        {
            PRA_START_EVENT_DATA start_event_data = {};
            start_event_data.mode = PRA_RUNE_BIG_MODE;
            start_event_data.color = PR_RED;
            start_event_data.big_group_index = 1;
            start_event_data.big_target_armed = 1;
            // 向后兼容：旧包长不包含group/armed字段，默认按group=1、armed=1处理
            if (event_data != NULL)
            {
                uint8_t data_len = ((uint8_t *)event_data)[1];
                size_t copy_len = data_len;
                if (copy_len == 0 || copy_len > sizeof(PRA_START_EVENT_DATA))
                    copy_len = sizeof(PRA_START_EVENT_DATA);
                if (copy_len < 4)
                    copy_len = 4; // 兼容旧结构最小长度(address,data_len,mode,color)
                memcpy(&start_event_data, event_data, copy_len);
                if (copy_len < sizeof(PRA_START_EVENT_DATA))
                {
                    start_event_data.big_group_index = 1;
                    start_event_data.big_target_armed = 1;
                }
            }
            trigger((RUNE_MODE)start_event_data.mode, (RUNE_COLOR)start_event_data.color,
                    start_event_data.big_group_index, start_event_data.big_target_armed);
            break;
        }
        case PRA_STOP_EVENT:
            stop();
            break;
        case PRA_HIT_EVENT:
        {
            // 检查状态机状态
            if (state.LED_Strip_State == LED_STRIP_TARGET)
            {
                if (state.mode == PRA_RUNE_ALL_TARGET_READY_MODE ||
                    state.mode == PRA_RUNE_ALL_SUCCESS_STATIC_MODE ||
                    state.mode == PRA_RUNE_SMALL_4_HIT_1_READY_TEST_MODE ||
                    state.mode == PRA_RUNE_BIG_PROGRESS_2_READY_TEST_MODE)
                {
                    ESP_LOGI(TAG_ARMOUR, "Ignore hit: all-panel mode keeps display static");
                    break;
                }
                if (state.mode == PRA_RUNE_SMALL_MODE && !big_rune_target_armed)
                {
                    ESP_LOGW(TAG_ARMOUR, "Ignore hit effect: small rune non-active panel");
                    break;
                }
                if (state.mode == PRA_RUNE_BIG_MODE && !big_rune_target_armed)
                {
                    ESP_LOGW(TAG_ARMOUR, "Ignore hit: big rune non-active panel, group=%d", big_rune_group_index);
                    break;
                }
                PRA_HIT_EVENT_DATA *hit_event_data = (PRA_HIT_EVENT_DATA *)event_data;
                hit(hit_event_data->score);
            }
            break;
        }
        case PRA_COMPLETE_EVENT:
        {
            if (state.LED_Strip_State == LED_STRIP_HIT)
            {
                pending_blink_after_hit = true;
                xSemaphoreGive(LED_Strip_FSM_Semaphore);
            }
            else
            {
                blink();
            }
            break;
        }
        }
    else if (base == PRC)
    {
        switch (id)
        {
        case OTA_BEGIN_EVENT:
            debug();
            break;
        case OTA_COMPLETE_EVENT:
            stop();
            break;
        }
    }
}
