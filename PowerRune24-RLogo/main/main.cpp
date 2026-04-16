/**
 * @file "main.cpp"
 * @note 本文件存放大符主控的操作逻辑代码
 */
#include "main.h"

#ifndef PR_MAIN_MOTOR_ONLY_TEST
#define PR_MAIN_MOTOR_ONLY_TEST 0
#endif

// 排序法生成不重复随机数列
void generate_rand_sequence(uint8_t *rand_sequence, int length)
{
    int i, j, temp;
    for (i = 0; i < length; i++)
    {
        rand_sequence[i] = i + 1;
    }
    for (i = 0; i < length; i++)
    {
        j = esp_random() % length;
        temp = rand_sequence[i];
        rand_sequence[i] = rand_sequence[j];
        rand_sequence[j] = temp;
    }
}

static bool wait_send_ack(const char *context, TickType_t timeout_ticks = (3000 / portTICK_PERIOD_MS))
{
    if (ESPNowProtocol::send_state == NULL)
    {
        ESP_LOGW(TAG_MAIN, "send_state is NULL while waiting ACK: %s", context);
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(ESPNowProtocol::send_state, ESPNowProtocol::SEND_ACK_OK_BIT, pdTRUE, pdTRUE, timeout_ticks);
    if ((bits & ESPNowProtocol::SEND_ACK_OK_BIT) == 0)
    {
        ESP_LOGW(TAG_MAIN, "Timeout waiting ACK: %s", context);
        return false;
    }
    return true;
}

// stop_task 与 run_task 间的轻量停止标志，用于中断长等待阶段
static volatile bool g_run_stop_requested = false;
// run_task 运行中收到新的 RUN 指令时，排队在当前轮结束后自动重启
static volatile bool g_run_restart_requested = false;
// 运行代号：用于避免旧 stop_task 误杀新 run_task
static volatile uint32_t g_run_generation = 0;
enum LOGO_ANIMATION_MODE : uint8_t
{
    LOGO_ANIM_IDLE_FLOW_GRADIENT = 0,
    LOGO_ANIM_MONO_BREATH = 1,
    LOGO_ANIM_SOLID = 2,
    LOGO_ANIM_OFF = 3,
};
static volatile uint8_t g_logo_animation_mode = LOGO_ANIM_IDLE_FLOW_GRADIENT;
static volatile uint8_t g_logo_animation_color = PR_RED;

static void prm_stop_done_event_handler_local(void *handler_args, esp_event_base_t base, int32_t id, void *event_data);

static inline uint8_t sanitize_logo_color(uint8_t color)
{
    return (color == PR_BLUE) ? PR_BLUE : PR_RED;
}

static inline void notify_logo_animation_task()
{
    if (led_animation_task_handle != NULL)
        xTaskNotifyGive(led_animation_task_handle);
}

static void set_logo_idle_flow_mode()
{
    g_logo_animation_mode = LOGO_ANIM_IDLE_FLOW_GRADIENT;
    notify_logo_animation_task();
}

static void set_logo_mono_breath_mode(uint8_t color)
{
    g_logo_animation_mode = LOGO_ANIM_MONO_BREATH;
    g_logo_animation_color = sanitize_logo_color(color);
    notify_logo_animation_task();
}

static void set_logo_solid_mode(uint8_t color)
{
    g_logo_animation_mode = LOGO_ANIM_SOLID;
    g_logo_animation_color = sanitize_logo_color(color);
    notify_logo_animation_task();
}

static void set_logo_off_mode()
{
    g_logo_animation_mode = LOGO_ANIM_OFF;
    notify_logo_animation_task();
}

static void clear_gpa_score_history()
{
    score_vector.clear();
    memset(ops_gpa_val, 0, sizeof(ops_gpa_val));
    esp_ble_gatts_set_attr_value(ops_handle_table[GPA_VAL], sizeof(ops_gpa_val), ops_gpa_val);
}

static void update_gpa_score_history(uint8_t score)
{
    score_vector.push_back(score);
    size_t len = score_vector.size();
    memset(ops_gpa_val, 0, sizeof(ops_gpa_val));
    for (size_t i = 0; i < sizeof(ops_gpa_val) && i < len; i++)
    {
        ops_gpa_val[i] = score_vector[len - 1 - i];
    }
    esp_ble_gatts_set_attr_value(ops_handle_table[GPA_VAL], sizeof(ops_gpa_val), ops_gpa_val);
}

void unlock_done_event_handler(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    SemaphoreHandle_t unlock_done_sem = (SemaphoreHandle_t)handler_args;
    PRM_UNLOCK_DONE_EVENT_DATA *data = (PRM_UNLOCK_DONE_EVENT_DATA *)event_data;
    // 打印事件数据
    ESP_LOGI(TAG_MAIN, "Motor Unlock Done, result: %s", esp_err_to_name(data->status));
    // 发送indicator日志
    char log_string[25];
    sprintf(log_string, (data->status == ESP_OK) ? "Motor unlocked." : "Fail to unlock motor.");
    esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[RUN_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);
    // 释放信号量
    xSemaphoreGive(unlock_done_sem);
    // 自注销
    esp_event_handler_unregister_with(pr_events_loop_handle, PRM, PRM_UNLOCK_DONE_EVENT, unlock_done_event_handler);
    return;
}

void unlock_task(void *pvParameter)
{
    char log_string[] = "Unlocking Motor...";
    esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[RUN_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);

    SemaphoreHandle_t unlock_done_sem = xSemaphoreCreateBinary();
    // 注册事件
    esp_event_handler_register_with(pr_events_loop_handle, PRM, PRM_UNLOCK_DONE_EVENT, unlock_done_event_handler, unlock_done_sem);
    // 发送START事件
    PRM_UNLOCK_EVENT_DATA prm_unlock_event_data;
    prm_unlock_event_data.address = MOTOR;
    esp_event_post_to(pr_events_loop_handle, PRM, PRM_UNLOCK_EVENT, &prm_unlock_event_data, sizeof(PRM_UNLOCK_EVENT_DATA), portMAX_DELAY);
    // 等待通信
    if (!wait_send_ack("PRM_UNLOCK_EVENT"))
    {
        vSemaphoreDelete(unlock_done_sem);
        vTaskDelete(NULL);
        return;
    }
    // 等待信号量
    if (xSemaphoreTake(unlock_done_sem, 10000 / portTICK_PERIOD_MS) != pdTRUE)
    {
        ESP_LOGW(TAG_MAIN, "Timeout waiting PRM_UNLOCK_DONE_EVENT");
    }
    vSemaphoreDelete(unlock_done_sem);
    vTaskDelete(NULL);
}

// PowerRune_Events handles
static void pra_stop(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    set_logo_idle_flow_mode();
}

void stop_task(void *pvParameter)
{
    uint32_t stop_target_generation = g_run_generation;
    TaskHandle_t stop_target_task = xTaskGetHandle("run_task");
    QueueHandle_t stop_target_queue = run_queue;
    TimerHandle_t stop_target_timer = hit_timer;
    QueueHandle_t stop_done_queue = xQueueCreate(1, sizeof(PRM_STOP_DONE_EVENT_DATA));
    bool stop_done_handler_registered = false;
    if (stop_done_queue != NULL)
    {
        esp_event_handler_register_with(pr_events_loop_handle, PRM, PRM_STOP_DONE_EVENT, prm_stop_done_event_handler_local, stop_done_queue);
        stop_done_handler_registered = true;
    }

    g_run_stop_requested = true;
    g_run_restart_requested = false;

    // 发送停止位
    for (uint8_t tries = 0; tries < 30; tries++)
    {
        if (g_run_generation != stop_target_generation)
        {
            ESP_LOGW(TAG_MAIN, "stop_task detected newer run_task, stop current stop sequence");
            break;
        }

        if (stop_target_task == NULL || eTaskGetState(stop_target_task) == eDeleted)
        {
            break;
        }

        ESP_LOGI(TAG_MAIN, "Sending STOP to run_task");
        if (stop_target_timer != NULL)
            xTimerStop(stop_target_timer, 0);

        // 非阻塞发送停止事件，避免 stop_task 被队列写满卡住
        PRA_HIT_EVENT_DATA hit_done_data = {};
        hit_done_data.address = 10;
        if (stop_target_queue != NULL)
        {
            if (xQueueSend(stop_target_queue, &hit_done_data, 0) != pdTRUE)
            {
                ESP_LOGW(TAG_MAIN, "run_queue full while stopping, retrying");
            }
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    // STOP
    char log_string[35] = "Stopping Armour...";
    bool stop_cancelled_by_new_run = (g_run_generation != stop_target_generation);
    PRA_STOP_EVENT_DATA pra_stop_event_data;
    for (uint8_t i = 0; i < 5 && !stop_cancelled_by_new_run; i++)
    {
        pra_stop_event_data.address = i;
        esp_event_post_to(pr_events_loop_handle, PRA, PRA_STOP_EVENT, &pra_stop_event_data, sizeof(PRA_STOP_EVENT_DATA), portMAX_DELAY);
        // 等待ACK
        wait_send_ack("PRA_STOP_EVENT");
    }
    pra_stop(NULL, NULL, 0, NULL);
    sprintf(log_string, "Armour stopped, stopping motor");
    esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[STOP_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);
    // 发送STOP到PRM
    bool motor_stop_done_ok = false;
    if (!stop_cancelled_by_new_run)
    {
        PRM_STOP_EVENT_DATA prm_stop_event_data;
        PRM_STOP_DONE_EVENT_DATA stop_done_data = {};
        prm_stop_event_data.address = MOTOR;
        if (stop_done_queue != NULL)
            xQueueReset(stop_done_queue);
        ESP_LOGI(TAG_MAIN, "Sending PRM_STOP_EVENT: stop_task");
        esp_event_post_to(pr_events_loop_handle, PRM, PRM_STOP_EVENT, &prm_stop_event_data, sizeof(PRM_STOP_EVENT_DATA), portMAX_DELAY);
        // 等待ACK
        if (wait_send_ack("PRM_STOP_EVENT(stop_task)"))
        {
            ESP_LOGI(TAG_MAIN, "PRM_STOP_EVENT ACK received, waiting PRM_STOP_DONE_EVENT: stop_task");
            if (stop_done_queue != NULL && xQueueReceive(stop_done_queue, &stop_done_data, 3000 / portTICK_PERIOD_MS) == pdTRUE)
            {
                ESP_LOGI(TAG_MAIN, "PRM_STOP_DONE_EVENT received: status=%s context=stop_task", esp_err_to_name(stop_done_data.status));
                motor_stop_done_ok = (stop_done_data.status == ESP_OK);
            }
            else
            {
                ESP_LOGW(TAG_MAIN, "Timeout waiting PRM_STOP_DONE_EVENT: stop_task");
            }
        }
        else
        {
            ESP_LOGW(TAG_MAIN, "PRM_STOP_EVENT ACK timeout: stop_task");
        }
    }
    else
    {
        ESP_LOGW(TAG_MAIN, "stop_task motor stop skipped because newer run_task detected");
    }

    if (motor_stop_done_ok)
    {
        sprintf(log_string, "Motor stopped");
        esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[STOP_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);
    }
    else if (!stop_cancelled_by_new_run)
    {
        ESP_LOGW(TAG_MAIN, "Motor stop not confirmed by PRM_STOP_DONE_EVENT in stop_task");
    }

    // 等待 run_task 真正退出，避免下一次 RUN 误判为仍在运行
    for (uint8_t tries = 0; tries < 30 && !stop_cancelled_by_new_run; tries++)
    {
        if (stop_target_task == NULL || eTaskGetState(stop_target_task) == eDeleted)
            break;
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    g_run_stop_requested = false;
    if (g_run_restart_requested)
    {
        TaskHandle_t run_task_handle = xTaskGetHandle("run_task");
        if (run_task_handle == NULL || eTaskGetState(run_task_handle) == eDeleted)
        {
            g_run_restart_requested = false;
            ESP_LOGI(TAG_MAIN, "Starting queued RUN after stop_task");
            xTaskCreate((TaskFunction_t)run_task, "run_task", 4096, NULL, 10, NULL);
        }
    }

    if (stop_done_handler_registered)
        esp_event_handler_unregister_with(pr_events_loop_handle, PRM, PRM_STOP_DONE_EVENT, prm_stop_done_event_handler_local);
    if (stop_done_queue != NULL)
        vQueueDelete(stop_done_queue);

    vTaskDelete(NULL);
}

// PRA_HIT_EVENT 事件处理函数
void hit_event_handler(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    if (handler_args == NULL || event_data == NULL)
    {
        return;
    }
    PRA_HIT_EVENT_DATA *data = (PRA_HIT_EVENT_DATA *)event_data;
    if (xQueueSend((QueueHandle_t)handler_args, data, 0) != pdTRUE)
    {
        ESP_LOGW(TAG_MAIN, "run_queue full, drop hit event addr=%u", data->address);
    }
}

void hit_timer_callback(TimerHandle_t xTimer)
{
    if (run_queue == NULL)
    {
        return;
    }
    PRA_HIT_EVENT_DATA hit_done_data = {};
    hit_done_data.address = 0xFF;
    if (xQueueSend(run_queue, &hit_done_data, 0) != pdTRUE)
    {
        ESP_LOGW(TAG_MAIN, "run_queue full, drop hit timer marker addr=%u", hit_done_data.address);
    }
}

void prm_speed_stable_event_handler(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    SemaphoreHandle_t motor_done_sem = (SemaphoreHandle_t)handler_args;
    // 释放信号量
    xSemaphoreGive(motor_done_sem);
    return;
}

void prm_start_done_event_handler(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    if (handler_args == NULL || event_data == NULL)
    {
        return;
    }
    QueueHandle_t start_done_queue = (QueueHandle_t)handler_args;
    PRM_START_DONE_EVENT_DATA *data = (PRM_START_DONE_EVENT_DATA *)event_data;
    xQueueOverwrite(start_done_queue, data);
}

void prm_unlock_done_event_handler(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    if (handler_args == NULL || event_data == NULL)
    {
        return;
    }
    QueueHandle_t unlock_done_queue = (QueueHandle_t)handler_args;
    PRM_UNLOCK_DONE_EVENT_DATA *data = (PRM_UNLOCK_DONE_EVENT_DATA *)event_data;
    xQueueOverwrite(unlock_done_queue, data);
}

static void queue_overwrite_prm_stop_done(void *handler_args, void *event_data)
{
    if (handler_args == NULL || event_data == NULL)
    {
        return;
    }
    QueueHandle_t stop_done_queue = (QueueHandle_t)handler_args;
    PRM_STOP_DONE_EVENT_DATA *data = (PRM_STOP_DONE_EVENT_DATA *)event_data;
    xQueueOverwrite(stop_done_queue, data);
}

void prm_stop_done_event_handler(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    queue_overwrite_prm_stop_done(handler_args, event_data);
}

static void prm_stop_done_event_handler_local(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    queue_overwrite_prm_stop_done(handler_args, event_data);
}

/**
 * @brief run_task
 * @note 大符运行任务代码
 */
void run_task(void *pvParameter)
{
    g_run_generation++;
    const u_int8_t *value;
    u_int16_t len;
    // 随机数列
    uint8_t rune_start_sequence[5];
    char log_string[192];
    PRA_HIT_EVENT_DATA hit_done_data = {0};
    bool circulation = false;
    uint8_t last_first_activation_armour = 0;
    PRM_STOP_EVENT_DATA prm_stop_event_data;
    prm_stop_event_data.address = MOTOR;
    PRM_START_DONE_EVENT_DATA start_done_data = {0};
    PRM_UNLOCK_DONE_EVENT_DATA unlock_done_data = {0};
    PRM_STOP_DONE_EVENT_DATA stop_done_data = {0};
    // 事件队列
    run_queue = xQueueCreate(5, sizeof(PRA_HIT_EVENT_DATA));
    // FreeRTOS计时器
    hit_timer = xTimerCreate("hit_timer", 2500 / portTICK_PERIOD_MS, pdFALSE, (void *)0, hit_timer_callback);

    bool prm_speed_handler_registered = false;
    bool prm_start_done_handler_registered = false;
    bool prm_unlock_done_handler_registered = false;
    bool prm_stop_done_handler_registered = false;
    bool pra_hit_handler_registered = false;
    // 电机信号量
    motor_done_sem = xSemaphoreCreateBinary();
    QueueHandle_t motor_start_done_queue = xQueueCreate(1, sizeof(PRM_START_DONE_EVENT_DATA));
    QueueHandle_t motor_unlock_done_queue = xQueueCreate(1, sizeof(PRM_UNLOCK_DONE_EVENT_DATA));
    QueueHandle_t motor_stop_done_queue = xQueueCreate(1, sizeof(PRM_STOP_DONE_EVENT_DATA));
    const TickType_t small_hit_window = 2500 / portTICK_PERIOD_MS;
    const TickType_t big_first_hit_window = 2500 / portTICK_PERIOD_MS;
    const TickType_t big_second_hit_window = 1000 / portTICK_PERIOD_MS;
    const TickType_t motor_stable_timeout = 5000 / portTICK_PERIOD_MS;
    const TickType_t motor_cmd_settle = (120 / portTICK_PERIOD_MS) > 0 ? (120 / portTICK_PERIOD_MS) : 1;
    const TickType_t wait_poll_ticks = (100 / portTICK_PERIOD_MS) > 0 ? (100 / portTICK_PERIOD_MS) : 1;
    g_run_stop_requested = false;

    auto stop_armours = [&](const uint8_t *addresses, uint8_t count) {
        PRA_STOP_EVENT_DATA pra_stop_event_data;
        for (uint8_t i = 0; i < count; i++)
        {
            pra_stop_event_data.address = addresses[i];
            esp_event_post_to(pr_events_loop_handle, PRA, PRA_STOP_EVENT, &pra_stop_event_data, sizeof(PRA_STOP_EVENT_DATA), portMAX_DELAY);
            wait_send_ack("PRA_STOP_EVENT");
        }
    };
    auto notify_test_hit_score = [&](const char *mode_tag, uint8_t address, uint8_t hit_score) {
        update_gpa_score_history(hit_score);
        ESP_LOGI(TAG_MAIN, "[%s] target armour %u hit score +%u", mode_tag, address + 1, hit_score);
        snprintf(log_string, sizeof(log_string), "[%s] Armour %u hit score +%u", mode_tag, address + 1, hit_score);
        esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[RUN_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);
    };
    // 注册事件
    esp_ble_gatts_get_attr_value(ops_handle_table[RUN_VAL], &len, &value);

    uint8_t run_color = PR_RED;
    uint8_t run_mode = PRA_RUNE_BIG_MODE;
    bool run_circulation = false;
    uint8_t run_direction = PRM_DIRECTION_CLOCKWISE;
    uint8_t run_test_leaf = 1;
    uint8_t run_big_ready_a = 1;
    uint8_t run_big_ready_b = 2;
    uint8_t run_small_progress = 0;
    if (value != NULL)
    {
        if (len >= 1)
            run_color = value[0];
        if (len >= 2)
            run_mode = value[1];
        if (len >= 3)
            run_circulation = (value[2] == 1);
        if (len >= 4)
            run_direction = value[3];
        if (len >= 5)
            run_test_leaf = value[4];
        if (len >= 6)
            run_big_ready_a = value[5];
        if (len >= 7)
            run_big_ready_b = value[6];
        if (len >= 8)
            run_small_progress = value[7];
    }
    if (run_color != PR_RED && run_color != PR_BLUE)
    {
        ESP_LOGW(TAG_MAIN, "Invalid run color %u, fallback to red", run_color);
        run_color = PR_RED;
    }
    if (run_mode != PRA_RUNE_BIG_MODE &&
        run_mode != PRA_RUNE_SMALL_MODE &&
        run_mode != PRA_RUNE_SINGLE_TEST_MODE &&
        run_mode != PRA_RUNE_ALL_TARGET_READY_MODE &&
        run_mode != PRA_RUNE_ALL_SUCCESS_STATIC_MODE &&
        run_mode != PRA_RUNE_SMALL_4_HIT_1_READY_TEST_MODE &&
        run_mode != PRA_RUNE_BIG_PROGRESS_2_READY_TEST_MODE &&
        run_mode != PRA_RUNE_SINGLE_SCORE_TEST_MODE &&
        run_mode != PRA_RUNE_AUTO_SUCCESS_MODE)
    {
        ESP_LOGW(TAG_MAIN, "Invalid run mode %u, fallback to big mode", run_mode);
        run_mode = PRA_RUNE_BIG_MODE;
    }
    if (run_direction != PRM_DIRECTION_CLOCKWISE &&
        run_direction != PRM_DIRECTION_ANTICLOCKWISE)
    {
        ESP_LOGW(TAG_MAIN, "Invalid run direction %u, fallback to clockwise", run_direction);
        run_direction = PRM_DIRECTION_CLOCKWISE;
    }
    if (run_test_leaf < 1 || run_test_leaf > 5)
    {
        ESP_LOGW(TAG_MAIN, "Invalid test leaf %u, fallback to 1", run_test_leaf);
        run_test_leaf = 1;
    }
    if (run_big_ready_a < 1 || run_big_ready_a > 5)
    {
        ESP_LOGW(TAG_MAIN, "Invalid big ready leaf A %u, fallback to 1", run_big_ready_a);
        run_big_ready_a = 1;
    }
    if (run_big_ready_b < 1 || run_big_ready_b > 5)
    {
        ESP_LOGW(TAG_MAIN, "Invalid big ready leaf B %u, fallback to 2", run_big_ready_b);
        run_big_ready_b = 2;
    }
    if (run_big_ready_a == run_big_ready_b)
    {
        run_big_ready_b = (run_big_ready_a % 5) + 1;
        ESP_LOGW(TAG_MAIN, "Big debug ready leaves duplicated, auto adjust to A=%u B=%u", run_big_ready_a, run_big_ready_b);
    }
    if (run_small_progress > 4)
    {
        ESP_LOGW(TAG_MAIN, "Invalid small debug progress %u, fallback to 0", run_small_progress);
        run_small_progress = 0;
    }

    const bool run_color_blue = (run_color == PR_BLUE);
    const bool run_mode_big = (run_mode == PRA_RUNE_BIG_MODE);
    const bool run_mode_small = (run_mode == PRA_RUNE_SMALL_MODE);
    const bool run_mode_single_test = (run_mode == PRA_RUNE_SINGLE_TEST_MODE);
    const bool run_mode_single_score_test = (run_mode == PRA_RUNE_SINGLE_SCORE_TEST_MODE);
    const bool run_mode_auto_success = (run_mode == PRA_RUNE_AUTO_SUCCESS_MODE);
    const bool run_mode_all_target_ready = (run_mode == PRA_RUNE_ALL_TARGET_READY_MODE);
    const bool run_mode_all_success_static = (run_mode == PRA_RUNE_ALL_SUCCESS_STATIC_MODE);
    const bool run_mode_small_4_hit_1_ready_test = (run_mode == PRA_RUNE_SMALL_4_HIT_1_READY_TEST_MODE);
    const bool run_mode_big_progress_2_ready_test = (run_mode == PRA_RUNE_BIG_PROGRESS_2_READY_TEST_MODE);
    const bool run_mode_small_profile = run_mode_small || run_mode_small_4_hit_1_ready_test;
    const bool run_mode_big_profile = run_mode_big || run_mode_big_progress_2_ready_test || run_mode_auto_success;
    const bool run_mode_no_motor = run_mode_single_test || run_mode_single_score_test || run_mode_all_target_ready || run_mode_all_success_static;
    const bool run_clockwise = (run_direction == PRM_DIRECTION_CLOCKWISE);
    const char *run_mode_text = "Big";
    switch (run_mode)
    {
    case PRA_RUNE_SMALL_MODE:
        run_mode_text = "Small";
        break;
    case PRA_RUNE_SINGLE_TEST_MODE:
        run_mode_text = "SingleTest";
        break;
    case PRA_RUNE_SINGLE_SCORE_TEST_MODE:
        run_mode_text = "SingleScoreTest";
        break;
    case PRA_RUNE_AUTO_SUCCESS_MODE:
        run_mode_text = "AutoSuccess";
        break;
    case PRA_RUNE_ALL_TARGET_READY_MODE:
        run_mode_text = "AllTargetReady";
        break;
    case PRA_RUNE_ALL_SUCCESS_STATIC_MODE:
        run_mode_text = "AllSuccessStatic";
        break;
    case PRA_RUNE_SMALL_4_HIT_1_READY_TEST_MODE:
        run_mode_text = "Small4Hit1ReadyTest";
        break;
    case PRA_RUNE_BIG_PROGRESS_2_READY_TEST_MODE:
        run_mode_text = "BigProgress2ReadyTest";
        break;
    default:
        break;
    }

    ESP_LOGI(TAG_MAIN, "Run Triggered:");
    ESP_LOGI(TAG_MAIN, "Color : %s", run_color_blue ? "Blue" : "Red");
    ESP_LOGI(TAG_MAIN, "Mode : %s", run_mode_text);
    ESP_LOGI(TAG_MAIN, "Circulation : %s", run_circulation ? "Enabled" : "Disabled");
    ESP_LOGI(TAG_MAIN, "Direction : %s", run_clockwise ? "Clockwise" : "Anti-Clockwise");
    ESP_LOGI(TAG_MAIN, "Test Leaf : %u", run_test_leaf);
    ESP_LOGI(TAG_MAIN, "Big Ready Leafs : %u,%u", run_big_ready_a, run_big_ready_b);
    ESP_LOGI(TAG_MAIN, "Small Progress : %u/4", run_small_progress);
    // 发送indicator日志
    snprintf(log_string, sizeof(log_string), "Run Color %s Mode %s Loop %s Dir %s Leaf %u BigReady %u,%u SmallProg %u/4",
             run_color_blue ? "Blue" : "Red",
             run_mode_text,
             run_circulation ? "Enabled" : "Disabled",
             run_clockwise ? "Clockwise" : "Anti-Clockwise",
             run_test_leaf,
             run_big_ready_a,
             run_big_ready_b,
             run_small_progress);
    esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[RUN_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);

    // 中心标：全靶待击打模式使用同色呼吸，其余模式固定同色常亮
    if (run_mode_all_target_ready)
        set_logo_mono_breath_mode(run_color);
    else
        set_logo_solid_mode(run_color);

    /* 生成随机速度参数
   速度目标函数为： spd = a ∗ sin(𝜔𝜔 ∗ 𝑡𝑡) + 𝑏𝑏，其中 spd 的单位
   为 rad/s， t 的单位为 s， a 的取值范围为 0.780~1.045，ω的取值范围为 1.884~2.000，
   b 始终满足 b=2.090-a。每次大能量机关进入可激活状态时，所有参数重置，
   其中 t 重置为 0， a 和ω重置为取值范围内任意值。*/

    // 注册事件
    if (!run_mode_no_motor)
    {
        esp_event_handler_unregister_with(pr_events_loop_handle, PRM, PRM_SPEED_STABLE_EVENT, prm_speed_stable_event_handler);
        esp_event_handler_register_with(pr_events_loop_handle, PRM, PRM_SPEED_STABLE_EVENT, prm_speed_stable_event_handler, motor_done_sem);
        prm_speed_handler_registered = true;
        esp_event_handler_unregister_with(pr_events_loop_handle, PRM, PRM_START_DONE_EVENT, prm_start_done_event_handler);
        esp_event_handler_register_with(pr_events_loop_handle, PRM, PRM_START_DONE_EVENT, prm_start_done_event_handler, motor_start_done_queue);
        prm_start_done_handler_registered = true;
        esp_event_handler_unregister_with(pr_events_loop_handle, PRM, PRM_UNLOCK_DONE_EVENT, prm_unlock_done_event_handler);
        esp_event_handler_register_with(pr_events_loop_handle, PRM, PRM_UNLOCK_DONE_EVENT, prm_unlock_done_event_handler, motor_unlock_done_queue);
        prm_unlock_done_handler_registered = true;
        esp_event_handler_unregister_with(pr_events_loop_handle, PRM, PRM_STOP_DONE_EVENT, prm_stop_done_event_handler);
        esp_event_handler_register_with(pr_events_loop_handle, PRM, PRM_STOP_DONE_EVENT, prm_stop_done_event_handler, motor_stop_done_queue);
        prm_stop_done_handler_registered = true;
    }

    auto wait_queue_or_stop = [&](QueueHandle_t q, void *out_data, TickType_t timeout_ticks) -> bool {
        TickType_t start = xTaskGetTickCount();
        while ((xTaskGetTickCount() - start) < timeout_ticks)
        {
            if (g_run_stop_requested)
                return false;
            TickType_t elapsed = xTaskGetTickCount() - start;
            TickType_t remain = timeout_ticks - elapsed;
            TickType_t chunk = (remain > wait_poll_ticks) ? wait_poll_ticks : remain;
            if (xQueueReceive(q, out_data, chunk) == pdTRUE)
                return true;
        }
        return false;
    };

    auto wait_sem_or_stop = [&](SemaphoreHandle_t sem, TickType_t timeout_ticks) -> bool {
        TickType_t start = xTaskGetTickCount();
        while ((xTaskGetTickCount() - start) < timeout_ticks)
        {
            if (g_run_stop_requested)
                return false;
            TickType_t elapsed = xTaskGetTickCount() - start;
            TickType_t remain = timeout_ticks - elapsed;
            TickType_t chunk = (remain > wait_poll_ticks) ? wait_poll_ticks : remain;
            if (xSemaphoreTake(sem, chunk) == pdTRUE)
                return true;
        }
        return false;
    };
    bool motor_need_unlock = true;

    auto start_motor_for_round = [&]() -> bool {
        if (g_run_stop_requested)
            return false;

        while (xSemaphoreTake(motor_done_sem, 0) == pdTRUE)
        {
        }

        PRM_START_EVENT_DATA prm_start_event_data;
        prm_start_event_data.clockwise = run_clockwise ? PRM_DIRECTION_CLOCKWISE : PRM_DIRECTION_ANTICLOCKWISE;
        if (run_mode_big_profile)
        {
            float a = (esp_random() % 266 + 780) / 1000.0;
            float omega = (esp_random() % 116 + 1884) / 1000.0;
            float b = 2.090 - a;
            prm_start_event_data.mode = PRA_RUNE_BIG_MODE;
            prm_start_event_data.amplitude = a;
            prm_start_event_data.omega = omega;
            prm_start_event_data.offset = b;
            ESP_LOGI(TAG_MAIN, "Starting motor in SIN tracing, amp = %f, omega = %f, b = %f", prm_start_event_data.amplitude, prm_start_event_data.omega, prm_start_event_data.offset);
            sprintf(log_string, "Starting motor in SIN tracing, amp = %f, omega = %f, b = %f", prm_start_event_data.amplitude, prm_start_event_data.omega, prm_start_event_data.offset);
        }
        else if (run_mode_small_profile)
        {
            prm_start_event_data.mode = PRA_RUNE_SMALL_MODE;
            ESP_LOGI(TAG_MAIN, "Starting motor in constant speed mode");
            sprintf(log_string, "Starting motor in constant speed mode");
        }
        else
        {
            prm_start_event_data.mode = PRA_RUNE_SMALL_MODE;
            ESP_LOGW(TAG_MAIN, "Unexpected run mode %u for motor profile, fallback to small constant speed", run_mode);
            sprintf(log_string, "Fallback motor profile: small constant speed");
        }
        esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[RUN_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);

        auto request_unlock = [&]() -> bool {
            xQueueReset(motor_unlock_done_queue);
            PRM_UNLOCK_EVENT_DATA prm_unlock_event_data = {};
            prm_unlock_event_data.address = MOTOR;
            ESP_LOGI(TAG_MAIN, "[PRM] TX UNLOCK");
            esp_event_post_to(pr_events_loop_handle, PRM, PRM_UNLOCK_EVENT, &prm_unlock_event_data, sizeof(PRM_UNLOCK_EVENT_DATA), portMAX_DELAY);
            if (!wait_send_ack("PRM_UNLOCK_EVENT(run_task)"))
            {
                ESP_LOGW(TAG_MAIN, "[PRM] ACK UNLOCK timeout");
                return false;
            }
            ESP_LOGI(TAG_MAIN, "[PRM] ACK UNLOCK");
            unlock_done_data = PRM_UNLOCK_DONE_EVENT_DATA{};
            if (!wait_queue_or_stop(motor_unlock_done_queue, &unlock_done_data, 3000 / portTICK_PERIOD_MS))
            {
                if (!g_run_stop_requested)
                    ESP_LOGW(TAG_MAIN, "[PRM] DONE UNLOCK timeout");
                return false;
            }
            ESP_LOGI(TAG_MAIN, "[PRM] DONE UNLOCK status=%s", esp_err_to_name(unlock_done_data.status));
            if (unlock_done_data.status != ESP_OK)
            {
                ESP_LOGW(TAG_MAIN, "[PRM] PRM_UNLOCK_DONE_EVENT status: %s", esp_err_to_name(unlock_done_data.status));
                return false;
            }
            return true;
        };

        auto request_start = [&](esp_err_t *status_out) -> bool {
            xQueueReset(motor_start_done_queue);
            ESP_LOGI(TAG_MAIN, "[PRM] TX START mode=%u dir=%u", prm_start_event_data.mode, prm_start_event_data.clockwise);
            esp_event_post_to(pr_events_loop_handle, PRM, PRM_START_EVENT, &prm_start_event_data, sizeof(PRM_START_EVENT_DATA), portMAX_DELAY);
            if (!wait_send_ack("PRM_START_EVENT"))
            {
                ESP_LOGW(TAG_MAIN, "[PRM] ACK START timeout");
                return false;
            }
            ESP_LOGI(TAG_MAIN, "[PRM] ACK START");
            start_done_data = PRM_START_DONE_EVENT_DATA{};
            if (!wait_queue_or_stop(motor_start_done_queue, &start_done_data, 3000 / portTICK_PERIOD_MS))
            {
                if (!g_run_stop_requested)
                    ESP_LOGW(TAG_MAIN, "[PRM] DONE START timeout");
                return false;
            }
            *status_out = start_done_data.status;
            ESP_LOGI(TAG_MAIN, "[PRM] DONE START status=%s mode=%u",
                     esp_err_to_name(start_done_data.status), start_done_data.mode);
            return true;
        };

        auto request_stop = [&](const char *ack_context) -> bool {
            if (motor_stop_done_queue == NULL)
            {
                ESP_LOGW(TAG_MAIN, "motor_stop_done_queue is NULL: %s", ack_context);
                return false;
            }
            xQueueReset(motor_stop_done_queue);
            PRM_STOP_EVENT_DATA stop_data = {};
            stop_data.address = MOTOR;
            ESP_LOGI(TAG_MAIN, "[PRM] TX STOP context=%s", ack_context);
            esp_event_post_to(pr_events_loop_handle, PRM, PRM_STOP_EVENT, &stop_data, sizeof(PRM_STOP_EVENT_DATA), portMAX_DELAY);
            if (!wait_send_ack(ack_context))
            {
                ESP_LOGW(TAG_MAIN, "[PRM] ACK STOP timeout context=%s", ack_context);
                return false;
            }
            ESP_LOGI(TAG_MAIN, "[PRM] ACK STOP context=%s", ack_context);
            stop_done_data = PRM_STOP_DONE_EVENT_DATA{};
            if (!wait_queue_or_stop(motor_stop_done_queue, &stop_done_data, 3000 / portTICK_PERIOD_MS))
            {
                if (!g_run_stop_requested)
                    ESP_LOGW(TAG_MAIN, "[PRM] DONE STOP timeout context=%s", ack_context);
                return false;
            }
            ESP_LOGI(TAG_MAIN, "[PRM] DONE STOP status=%s context=%s", esp_err_to_name(stop_done_data.status), ack_context);
            if (stop_done_data.status != ESP_OK)
            {
                ESP_LOGW(TAG_MAIN, "[PRM] DONE STOP status not OK: %s context=%s", esp_err_to_name(stop_done_data.status), ack_context);
                return false;
            }
            vTaskDelay(motor_cmd_settle);
            motor_need_unlock = true;
            ESP_LOGI(TAG_MAIN, "[PRM] STOP completed, allow UNLOCK/START context=%s", ack_context);
            return true;
        };
        (void)request_stop; // Keep helper for logging/diagnosis while recovery chain is disabled.

        for (uint8_t attempt = 0; attempt < 2; attempt++)
        {
            // 循环模式下避免每一轮都UNLOCK导致电机瞬停，仅在首次或恢复后解锁
            if (motor_need_unlock)
            {
                if (!request_unlock())
                    return false;
                motor_need_unlock = false;
            }

            esp_err_t start_status = ESP_FAIL;
            if (!request_start(&start_status))
                return false;

            if (start_status == ESP_ERR_NOT_SUPPORTED)
            {
                ESP_LOGW(TAG_MAIN, "PRM_START_DONE_EVENT status: ESP_ERR_NOT_SUPPORTED, recovery disabled, abort this round");
                return false;
            }

            if (start_status != ESP_OK)
            {
                ESP_LOGW(TAG_MAIN, "PRM_START_DONE_EVENT status: %s", esp_err_to_name(start_status));
                return false;
            }

            while (xSemaphoreTake(motor_done_sem, 0) == pdTRUE)
            {
            }
            if (wait_sem_or_stop(motor_done_sem, motor_stable_timeout))
            {
                ESP_LOGI(TAG_MAIN, "[PRM] SPEED STABLE");
                sprintf(log_string, "PRM speed stable.");
                esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[RUN_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);
                return true;
            }

            if (g_run_stop_requested)
                return false;

            ESP_LOGW(TAG_MAIN, "PRM_SPEED_STABLE_EVENT timeout(%lu ms), recovery disabled, abort this round",
                     (unsigned long)(motor_stable_timeout * portTICK_PERIOD_MS));
            return false;
        }

        return false;
    };

#if !PR_MAIN_MOTOR_ONLY_TEST
    // 事件、等待PRA_HIT_EVENT信号量
    esp_event_handler_unregister_with(pr_events_loop_handle, PRA, PRA_HIT_EVENT, hit_event_handler);
    esp_event_handler_register_with(pr_events_loop_handle, PRA, PRA_HIT_EVENT, hit_event_handler, run_queue);
    pra_hit_handler_registered = true;
#else
    ESP_LOGW(TAG_MAIN, "[MOTOR_ONLY_TEST] PR_MAIN_MOTOR_ONLY_TEST=1, skip PRA_HIT_EVENT flow");
#endif
    circulation = run_circulation;
    bool motor_stopped = false;
    bool motor_running = false;

    auto stop_motor_reliably = [&](const char *ack_context, uint8_t retries = 3) -> bool {
        if (motor_stop_done_queue == NULL)
        {
            ESP_LOGW(TAG_MAIN, "motor_stop_done_queue is NULL: %s", ack_context);
            return false;
        }
        for (uint8_t i = 0; i < retries; i++)
        {
            bool ack_received = false;
            bool done_received = false;
            esp_err_t done_status = ESP_FAIL;
            xQueueReset(motor_stop_done_queue);
            prm_stop_event_data.address = MOTOR;
            ESP_LOGI(TAG_MAIN, "[PRM] TX STOP context=%s retry=%u/%u", ack_context, i + 1, retries);
            esp_event_post_to(pr_events_loop_handle, PRM, PRM_STOP_EVENT, &prm_stop_event_data, sizeof(PRM_STOP_EVENT_DATA), portMAX_DELAY);
            ack_received = wait_send_ack(ack_context, 1200 / portTICK_PERIOD_MS);
            if (!ack_received)
            {
                ESP_LOGW(TAG_MAIN, "[PRM] STOP context=%s retry=%u/%u ack=%s done=%s done_status=N/A",
                         ack_context, i + 1, retries, ack_received ? "yes" : "no", done_received ? "yes" : "no");
                vTaskDelay(80 / portTICK_PERIOD_MS);
                continue;
            }
            ESP_LOGI(TAG_MAIN, "[PRM] ACK STOP context=%s retry=%u/%u", ack_context, i + 1, retries);
            stop_done_data = PRM_STOP_DONE_EVENT_DATA{};
            done_received = wait_queue_or_stop(motor_stop_done_queue, &stop_done_data, 3000 / portTICK_PERIOD_MS);
            if (!done_received)
            {
                if (!g_run_stop_requested)
                    ESP_LOGW(TAG_MAIN, "[PRM] STOP context=%s retry=%u/%u ack=%s done=%s done_status=N/A",
                             ack_context, i + 1, retries, ack_received ? "yes" : "no", done_received ? "yes" : "no");
                vTaskDelay(80 / portTICK_PERIOD_MS);
                continue;
            }
            done_status = stop_done_data.status;
            ESP_LOGI(TAG_MAIN, "[PRM] DONE STOP context=%s retry=%u/%u ack=%s done=%s done_status=%s",
                     ack_context, i + 1, retries,
                     ack_received ? "yes" : "no",
                     done_received ? "yes" : "no",
                     esp_err_to_name(done_status));
            if (done_status != ESP_OK)
            {
                ESP_LOGW(TAG_MAIN, "[PRM] DONE STOP status not OK context=%s retry=%u/%u ack=%s done=%s done_status=%s",
                         ack_context, i + 1, retries,
                         ack_received ? "yes" : "no",
                         done_received ? "yes" : "no",
                         esp_err_to_name(done_status));
                vTaskDelay(80 / portTICK_PERIOD_MS);
                continue;
            }
            motor_need_unlock = true;
            motor_running = false;
            ESP_LOGI(TAG_MAIN, "[PRM] STOP completed, allow UNLOCK/START context=%s retry=%u/%u ack=%s done=%s done_status=%s",
                     ack_context, i + 1, retries,
                     ack_received ? "yes" : "no",
                     done_received ? "yes" : "no",
                     esp_err_to_name(done_status));
            return true;
        }
        return false;
    };
    do
    {
        bool round_success = true;
        bool user_stop = false;
        bool suppress_round_result_log = false;
        uint8_t score = 0;
        xQueueReset(run_queue);
#if PR_MAIN_MOTOR_ONLY_TEST
        if (!start_motor_for_round())
        {
            ESP_LOGW(TAG_MAIN, "[MOTOR_ONLY_TEST] start_motor_for_round failed, abort");
            circulation = 0;
            break;
        }
        motor_running = true;
        ESP_LOGI(TAG_MAIN, "motor only test running...");
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        if (stop_motor_reliably("PRM_STOP_EVENT(motor_only_test)"))
        {
            motor_stopped = true;
        }
        else
        {
            ESP_LOGW(TAG_MAIN, "[MOTOR_ONLY_TEST] stop failed: PRM_STOP_EVENT ACK/DONE missing");
        }
        circulation = 0;
        continue;
#endif
        if (run_mode_single_test)
        {
            suppress_round_result_log = true;
            const uint8_t target_address = (uint8_t)(run_test_leaf - 1);
            PRA_START_EVENT_DATA single_start_event_data = {
                .address = target_address,
                .data_len = sizeof(PRA_START_EVENT_DATA),
                .mode = PRA_RUNE_SMALL_MODE,
                .color = run_color,
            };
            PRA_STOP_EVENT_DATA single_stop_event_data = {
                .address = target_address,
                .data_len = sizeof(PRA_STOP_EVENT_DATA),
            };
            auto arm_single_target = [&]() -> bool {
                esp_event_post_to(pr_events_loop_handle, PRA, PRA_START_EVENT, &single_start_event_data, sizeof(PRA_START_EVENT_DATA), portMAX_DELAY);
                if (!wait_send_ack("PRA_START_EVENT(single_test)"))
                {
                    ESP_LOGW(TAG_MAIN, "[SINGLE_TEST] PRA_START_EVENT ACK timeout");
                    return false;
                }
                return true;
            };

            if (!arm_single_target())
            {
                circulation = false;
                break;
            }

            ESP_LOGI(TAG_MAIN, "[SINGLE_TEST] armed on armour %u", target_address + 1);
            sprintf(log_string, "Single test armed: armour %u", target_address + 1);
            esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[RUN_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);

            while (!g_run_stop_requested)
            {
                if (!wait_queue_or_stop(run_queue, &hit_done_data, small_hit_window))
                {
                    if (g_run_stop_requested)
                    {
                        user_stop = true;
                        break;
                    }
                    continue;
                }

                if (hit_done_data.address == 10)
                {
                    user_stop = true;
                    break;
                }

                if (hit_done_data.address == 0xFF)
                    continue;

                if (hit_done_data.address != target_address)
                {
                    ESP_LOGW(TAG_MAIN, "[SINGLE_TEST] ignore hit armour %u, target %u",
                             hit_done_data.address + 1, target_address + 1);
                    continue;
                }

                ESP_LOGI(TAG_MAIN, "[SINGLE_TEST] target armour %u hit score +%u", target_address + 1, hit_done_data.score);
                sprintf(log_string, "[SingleTest] Armour %u hit score +%u", target_address + 1, hit_done_data.score);
                esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[RUN_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);

                if (!arm_single_target())
                {
                    round_success = false;
                    circulation = false;
                    break;
                }
            }

            esp_event_post_to(pr_events_loop_handle, PRA, PRA_STOP_EVENT, &single_stop_event_data, sizeof(PRA_STOP_EVENT_DATA), portMAX_DELAY);
            wait_send_ack("PRA_STOP_EVENT(single_test)");
            circulation = false;
            round_success = false;
        }
        else if (run_mode_single_score_test)
        {
            suppress_round_result_log = true;
            const uint8_t all_addr[5] = {0, 1, 2, 3, 4};
            const uint8_t target_address = (uint8_t)(run_test_leaf - 1);
            PRA_START_EVENT_DATA single_score_start_event_data = {
                .address = target_address,
                .data_len = sizeof(PRA_START_EVENT_DATA),
                .mode = PRA_RUNE_SMALL_MODE,
                .color = run_color,
            };
            auto arm_single_score_target = [&]() -> bool {
                esp_event_post_to(pr_events_loop_handle, PRA, PRA_START_EVENT, &single_score_start_event_data, sizeof(PRA_START_EVENT_DATA), portMAX_DELAY);
                if (!wait_send_ack("PRA_START_EVENT(single_score_test)"))
                {
                    ESP_LOGW(TAG_MAIN, "[SINGLE_SCORE_TEST] PRA_START_EVENT ACK timeout");
                    return false;
                }
                return true;
            };

            clear_gpa_score_history();
            stop_armours(all_addr, 5);
            if (!arm_single_score_target())
            {
                circulation = false;
                round_success = false;
                break;
            }

            ESP_LOGI(TAG_MAIN, "[SINGLE_SCORE_TEST] armed on armour %u", target_address + 1);
            snprintf(log_string, sizeof(log_string), "Single score test armed: armour %u", target_address + 1);
            esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[RUN_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);

            while (!g_run_stop_requested)
            {
                if (!wait_queue_or_stop(run_queue, &hit_done_data, small_hit_window))
                {
                    if (g_run_stop_requested)
                    {
                        user_stop = true;
                        break;
                    }
                    continue;
                }

                if (hit_done_data.address == 10)
                {
                    user_stop = true;
                    break;
                }

                if (hit_done_data.address == 0xFF)
                    continue;

                if (hit_done_data.address != target_address)
                {
                    ESP_LOGW(TAG_MAIN, "[SINGLE_SCORE_TEST] ignore hit armour %u, target %u",
                             hit_done_data.address + 1, target_address + 1);
                    continue;
                }

                update_gpa_score_history(hit_done_data.score);
                ESP_LOGI(TAG_MAIN, "[SINGLE_SCORE_TEST] target armour %u hit score +%u", target_address + 1, hit_done_data.score);
                snprintf(log_string, sizeof(log_string), "[SingleScoreTest] Armour %u hit score +%u", target_address + 1, hit_done_data.score);
                esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[RUN_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);

                if (!arm_single_score_target())
                {
                    round_success = false;
                    circulation = false;
                    break;
                }
            }

            stop_armours(all_addr, 5);
            circulation = false;
            round_success = false;
        }
        else if (run_mode_small_4_hit_1_ready_test)
        {
            suppress_round_result_log = true;
            const uint8_t all_addr[5] = {0, 1, 2, 3, 4};
            uint8_t ready_leaf = run_test_leaf;
            if (ready_leaf < 1)
                ready_leaf = 1;
            if (ready_leaf > 5)
                ready_leaf = 5;
            uint8_t progress_leaf_count = run_small_progress;
            if (progress_leaf_count > 4)
                progress_leaf_count = 4;
            const uint8_t ready_address = ready_leaf - 1;
            bool small_static_mask[5] = {false, false, false, false, false};
            uint8_t filled = 0;
            for (uint8_t i = 0; i < 5 && filled < progress_leaf_count; i++)
            {
                if (i == ready_address)
                    continue;
                small_static_mask[i] = true;
                filled++;
            }
            PRA_START_EVENT_DATA small_test_start_event_data = {
                .address = 0,
                .data_len = sizeof(PRA_START_EVENT_DATA),
                .mode = PRA_RUNE_SMALL_4_HIT_1_READY_TEST_MODE,
                .color = run_color,
                .big_group_index = 1,
                .big_target_armed = 1,
            };
            auto apply_small_test_layout = [&]() -> bool {
                for (uint8_t i = 0; i < 5; i++)
                {
                    if (i == ready_address || small_static_mask[i])
                    {
                        small_test_start_event_data.address = i;
                        small_test_start_event_data.mode = (i == ready_address) ? PRA_RUNE_SMALL_4_HIT_1_READY_TEST_MODE : PRA_RUNE_SMALL_HIT_STATIC_MODE;
                        esp_event_post_to(pr_events_loop_handle, PRA, PRA_START_EVENT, &small_test_start_event_data, sizeof(PRA_START_EVENT_DATA), portMAX_DELAY);
                        if (!wait_send_ack("PRA_START_EVENT(small_progress_ready)"))
                            return false;
                    }
                    else
                    {
                        PRA_STOP_EVENT_DATA small_test_stop_event_data = {
                            .address = i,
                        };
                        esp_event_post_to(pr_events_loop_handle, PRA, PRA_STOP_EVENT, &small_test_stop_event_data, sizeof(PRA_STOP_EVENT_DATA), portMAX_DELAY);
                        if (!wait_send_ack("PRA_STOP_EVENT(small_progress_ready)"))
                            return false;
                    }
                }
                return true;
            };

            if (!start_motor_for_round())
            {
                ESP_LOGW(TAG_MAIN, "[SMALL_TEST_FIXED] start_motor_for_round failed, abort");
                round_success = false;
                circulation = false;
            }
            else
            {
                motor_running = true;
            }

            if (round_success && !apply_small_test_layout())
            {
                round_success = false;
                circulation = false;
            }

            if (round_success)
            {
                ESP_LOGI(TAG_MAIN, "Small test staged layout armed: progress=%u/4 ready=%u", progress_leaf_count, ready_address + 1);
                snprintf(log_string, sizeof(log_string), "Small test staged: progress=%u/4 ready=%u", progress_leaf_count, ready_address + 1);
                esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[RUN_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);

                while (!g_run_stop_requested)
                {
                    if (!wait_queue_or_stop(run_queue, &hit_done_data, 500 / portTICK_PERIOD_MS))
                    {
                        if (g_run_stop_requested)
                        {
                            user_stop = true;
                            break;
                        }
                        continue;
                    }

                    if (hit_done_data.address == 10)
                    {
                        user_stop = true;
                        break;
                    }

                    if (hit_done_data.address == 0xFF)
                        continue;

                    if (hit_done_data.address != ready_address)
                    {
                        ESP_LOGI(TAG_MAIN, "[SMALL_TEST_FIXED] ignore hit armour %u score %u", hit_done_data.address + 1, hit_done_data.score);
                        continue;
                    }

                    notify_test_hit_score("Small4Hit1ReadyTest", ready_address, hit_done_data.score);
                    if (!apply_small_test_layout())
                    {
                        round_success = false;
                        circulation = false;
                        break;
                    }
                }
            }

            stop_armours(all_addr, 5);
            circulation = false;
            round_success = false;
        }
        else if (run_mode_big_progress_2_ready_test)
        {
            suppress_round_result_log = true;
            const uint8_t all_addr[5] = {0, 1, 2, 3, 4};
            uint8_t progress_step = run_test_leaf;
            if (progress_step < 1)
                progress_step = 1;
            if (progress_step > 5)
                progress_step = 5;
            const uint8_t progress_stage = progress_step - 1;
            const uint8_t ready_a_addr = run_big_ready_a - 1;
            const uint8_t ready_b_addr = run_big_ready_b - 1;

            PRA_START_EVENT_DATA big_test_start_event_data = {
                .address = 0,
                .data_len = sizeof(PRA_START_EVENT_DATA),
                .mode = PRA_RUNE_BIG_PROGRESS_2_READY_TEST_MODE,
                .color = run_color,
                .big_group_index = progress_step,
                .big_target_armed = 0,
            };
            auto apply_big_test_layout = [&]() -> bool {
                for (uint8_t i = 0; i < 5; i++)
                {
                    big_test_start_event_data.address = i;
                    big_test_start_event_data.big_target_armed = (i == ready_a_addr || i == ready_b_addr) ? 1 : 0;
                    esp_event_post_to(pr_events_loop_handle, PRA, PRA_START_EVENT, &big_test_start_event_data, sizeof(PRA_START_EVENT_DATA), portMAX_DELAY);
                    if (!wait_send_ack("PRA_START_EVENT(big_progress_2_ready)"))
                        return false;
                }
                return true;
            };

            if (!start_motor_for_round())
            {
                ESP_LOGW(TAG_MAIN, "[BIG_TEST_STAGED] start_motor_for_round failed, abort");
                round_success = false;
                circulation = false;
            }
            else
            {
                motor_running = true;
            }

            if (round_success && !apply_big_test_layout())
            {
                round_success = false;
                circulation = false;
            }

            if (round_success)
            {
                ESP_LOGI(TAG_MAIN, "Big test staged layout armed: progress=%u/5 ready=%u,%u", progress_stage, ready_a_addr + 1, ready_b_addr + 1);
                snprintf(log_string, sizeof(log_string), "Big test staged: progress=%u/5 ready=%u,%u", progress_stage, ready_a_addr + 1, ready_b_addr + 1);
                esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[RUN_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);

                while (!g_run_stop_requested)
                {
                    if (!wait_queue_or_stop(run_queue, &hit_done_data, 500 / portTICK_PERIOD_MS))
                    {
                        if (g_run_stop_requested)
                        {
                            user_stop = true;
                            break;
                        }
                        continue;
                    }

                    if (hit_done_data.address == 10)
                    {
                        user_stop = true;
                        break;
                    }

                    if (hit_done_data.address == 0xFF)
                        continue;

                    if (hit_done_data.address != ready_a_addr && hit_done_data.address != ready_b_addr)
                    {
                        ESP_LOGI(TAG_MAIN, "[BIG_TEST_STAGED] ignore hit armour %u score %u", hit_done_data.address + 1, hit_done_data.score);
                        continue;
                    }

                    notify_test_hit_score("BigProgress2ReadyTest", hit_done_data.address, hit_done_data.score);
                    if (!apply_big_test_layout())
                    {
                        round_success = false;
                        circulation = false;
                        break;
                    }
                }
            }

            stop_armours(all_addr, 5);
            circulation = false;
            round_success = false;
        }
        else if (run_mode_all_target_ready || run_mode_all_success_static)
        {
            const uint8_t all_addr[5] = {0, 1, 2, 3, 4};
            PRA_START_EVENT_DATA all_start_event_data = {
                .address = 0,
                .data_len = sizeof(PRA_START_EVENT_DATA),
                .mode = run_mode,
                .color = run_color,
                .big_group_index = 1,
                .big_target_armed = 1,
            };

            for (uint8_t i = 0; i < 5; i++)
            {
                all_start_event_data.address = i;
                esp_event_post_to(pr_events_loop_handle, PRA, PRA_START_EVENT, &all_start_event_data, sizeof(PRA_START_EVENT_DATA), portMAX_DELAY);
                if (!wait_send_ack("PRA_START_EVENT(all_modes)"))
                {
                    round_success = false;
                    circulation = false;
                    break;
                }
            }

            if (round_success)
            {
                const char *mode_log = run_mode_all_target_ready ? "All target ready mode armed" : "All success static mode armed";
                ESP_LOGI(TAG_MAIN, "%s", mode_log);
                snprintf(log_string, sizeof(log_string), "%s", mode_log);
                esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[RUN_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);

                while (!g_run_stop_requested)
                {
                    if (!wait_queue_or_stop(run_queue, &hit_done_data, 500 / portTICK_PERIOD_MS))
                    {
                        if (g_run_stop_requested)
                        {
                            user_stop = true;
                            break;
                        }
                        continue;
                    }

                    if (hit_done_data.address == 10)
                    {
                        user_stop = true;
                        break;
                    }

                    if (hit_done_data.address == 0xFF)
                        continue;

                    ESP_LOGI(TAG_MAIN, "[ALL_MODE] ignore hit armour %u score %u", hit_done_data.address + 1, hit_done_data.score);
                }
            }

            stop_armours(all_addr, 5);
            circulation = false;
            round_success = false;
        }
        else
        {
            bool need_start_this_round = true;
            if (run_mode_small && circulation && motor_running)
            {
                need_start_this_round = false;
                ESP_LOGI(TAG_MAIN, "Small circulation: keep motor running for next round");
            }

            if (need_start_this_round)
            {
                if (!start_motor_for_round())
                {
                    ESP_LOGW(TAG_MAIN, "start_motor_for_round failed, recovery disabled, abort this round");
                    circulation = 0;
                    break;
                }
                motor_running = true;
            }
            PRA_START_EVENT_DATA pra_start_event_data = {
                .address = 0,
                .data_len = sizeof(PRA_START_EVENT_DATA),
                .mode = run_mode,
                .color = run_color,
            };
            PRA_COMPLETE_EVENT_DATA pra_complete_event_data;
            const uint8_t all_addr[5] = {0, 1, 2, 3, 4};
            constexpr uint8_t complete_ack_max_retry = 3;
            auto drain_run_queue = [&]() {
                if (run_queue == NULL)
                    return;
                PRA_HIT_EVENT_DATA stale_hit_data = {};
                while (xQueueReceive(run_queue, &stale_hit_data, 0) == pdTRUE)
                {
                    if (stale_hit_data.address != 0xFF)
                    {
                        ESP_LOGW(TAG_MAIN, "[HIT] drain stale event addr=%u score=%u", stale_hit_data.address, stale_hit_data.score);
                    }
                }
            };
            auto send_complete_with_retry = [&](uint8_t address) -> bool {
                for (uint8_t retry = 0; retry < complete_ack_max_retry; retry++)
                {
                    pra_complete_event_data.address = address;
                    esp_event_post_to(pr_events_loop_handle, PRA, PRA_COMPLETE_EVENT, &pra_complete_event_data, sizeof(PRA_COMPLETE_EVENT_DATA), portMAX_DELAY);
                    if (wait_send_ack("PRA_COMPLETE_EVENT"))
                        return true;
                    ESP_LOGW(TAG_MAIN, "PRA_COMPLETE_EVENT ACK timeout on armour %u retry %u/%u",
                             address + 1, retry + 1, complete_ack_max_retry);
                }
                return false;
            };
            auto wait_window_ignoring_hits = [&](TickType_t window_ticks, const char *window_tag) -> bool {
                TickType_t begin = xTaskGetTickCount();
                while ((xTaskGetTickCount() - begin) < window_ticks)
                {
                    if (g_run_stop_requested)
                    {
                        user_stop = true;
                        circulation = 0;
                        round_success = false;
                        return false;
                    }

                    TickType_t elapsed = xTaskGetTickCount() - begin;
                    TickType_t remain = window_ticks - elapsed;
                    TickType_t chunk = (remain > wait_poll_ticks) ? wait_poll_ticks : remain;
                    if (xQueueReceive(run_queue, &hit_done_data, chunk) != pdTRUE)
                        continue;

                    if (hit_done_data.address == 10)
                    {
                        user_stop = true;
                        circulation = 0;
                        round_success = false;
                        return false;
                    }

                    if (hit_done_data.address == 0xFF)
                        continue;

                    ESP_LOGI(TAG_MAIN, "[AUTO_SUCCESS] ignore hit armour %u score %u during %s window",
                             hit_done_data.address + 1, hit_done_data.score, window_tag);
                }
                return true;
            };

            if (run_mode == PRA_RUNE_SMALL_MODE)
            {
                // 小符模式：5块全部命中才成功
                do
                    generate_rand_sequence(rune_start_sequence, 5);
                while (rune_start_sequence[0] == last_first_activation_armour);
                last_first_activation_armour = rune_start_sequence[0];

                ESP_LOGI(TAG_MAIN, "Small Sequence: %i, %i, %i, %i, %i",
                         rune_start_sequence[0], rune_start_sequence[1], rune_start_sequence[2], rune_start_sequence[3], rune_start_sequence[4]);

                pra_start_event_data.big_group_index = 1;
                bool small_hit_mask[5] = {false, false, false, false, false};
                auto apply_small_round_layout = [&](uint8_t expected_address) -> bool {
                    for (uint8_t addr = 0; addr < 5; addr++)
                    {
                        pra_start_event_data.address = addr;
                        if (small_hit_mask[addr])
                        {
                            pra_start_event_data.mode = PRA_RUNE_SMALL_HIT_STATIC_MODE;
                            pra_start_event_data.big_target_armed = 1;
                        }
                        else
                        {
                            pra_start_event_data.mode = PRA_RUNE_SMALL_MODE;
                            pra_start_event_data.big_target_armed = (addr == expected_address) ? 1 : 0;
                        }
                        esp_event_post_to(pr_events_loop_handle, PRA, PRA_START_EVENT, &pra_start_event_data, sizeof(PRA_START_EVENT_DATA), portMAX_DELAY);
                        if (!wait_send_ack("PRA_START_EVENT"))
                            return false;
                    }
                    return true;
                };
                for (uint8_t step = 0; step < 5; step++)
                {
                    uint8_t expected_address = rune_start_sequence[step] - 1;
                    if (!apply_small_round_layout(expected_address))
                    {
                        round_success = false;
                        circulation = 0;
                        break;
                    }

                    // 每步开始前清空残留命中，避免上一步尾包干扰当前目标判定
                    drain_run_queue();
                    bool got_expected_hit = false;
                    TickType_t step_begin = xTaskGetTickCount();
                    while ((xTaskGetTickCount() - step_begin) < small_hit_window)
                    {
                        TickType_t step_elapsed = xTaskGetTickCount() - step_begin;
                        TickType_t step_remain = small_hit_window - step_elapsed;
                        if (!wait_queue_or_stop(run_queue, &hit_done_data, step_remain))
                        {
                            if (g_run_stop_requested)
                            {
                                user_stop = true;
                                circulation = 0;
                                round_success = false;
                            }
                            break;
                        }

                        if (hit_done_data.address == 10)
                        {
                            user_stop = true;
                            circulation = 0;
                            round_success = false;
                            break;
                        }

                        if (hit_done_data.address == expected_address)
                        {
                            score += hit_done_data.score;
                            ESP_LOGI(TAG_MAIN, "[HIT] armour %d score +%d", expected_address + 1, hit_done_data.score);
                            small_hit_mask[expected_address] = true;
                            got_expected_hit = true;
                            break;
                        }

                        if (hit_done_data.address == 0xFF)
                            continue;

                        ESP_LOGI(TAG_MAIN, "[HIT] ignore non-target armour %d, expected %d",
                                 hit_done_data.address + 1, expected_address + 1);
                        continue;
                    }
                    if (!round_success)
                        break;
                    if (!got_expected_hit)
                    {
                        ESP_LOGW(TAG_MAIN, "[HIT] timeout waiting target armour %d", expected_address + 1);
                        round_success = false;
                        break;
                    }
                }

                if (!round_success)
                {
                    stop_armours(all_addr, 5);
                }
                else
                {
                    bool complete_delivery_ok = true;
                    for (uint8_t i = 0; i < 5; i++)
                    {
                        if (!send_complete_with_retry(i))
                        {
                            complete_delivery_ok = false;
                            ESP_LOGW(TAG_MAIN, "Small mode COMPLETE delivery failed on armour %u", i + 1);
                            break;
                        }
                    }
                    if (!complete_delivery_ok)
                    {
                        round_success = false;
                    }
                    if (round_success)
                    {
                        vTaskDelay(5000 / portTICK_PERIOD_MS);
                    }
                    stop_armours(all_addr, 5);
                }
            }
            else if (run_mode_auto_success)
            {
                // 自动成功模式：复用大符正式节奏，但所有检测窗口默认通过并在最后显示成功等效
                pra_start_event_data.mode = PRA_RUNE_BIG_MODE;
                for (uint8_t group = 0; group < 5; group++)
                {
                    generate_rand_sequence(rune_start_sequence, 5);
                    uint8_t active_addr[2] = {(uint8_t)(rune_start_sequence[0] - 1), (uint8_t)(rune_start_sequence[1] - 1)};
                    ESP_LOGI(TAG_MAIN, "AutoSuccess Group %u targets: %u, %u", group + 1, active_addr[0] + 1, active_addr[1] + 1);
                    snprintf(log_string, sizeof(log_string), "AutoSuccess Group %u/5 targets: %u,%u", group + 1, active_addr[0] + 1, active_addr[1] + 1);
                    esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[RUN_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);

                    for (uint8_t addr = 0; addr < 5; addr++)
                    {
                        pra_start_event_data.address = addr;
                        pra_start_event_data.big_group_index = group + 1;
                        pra_start_event_data.big_target_armed = (addr == active_addr[0] || addr == active_addr[1]) ? 1 : 0;
                        esp_event_post_to(pr_events_loop_handle, PRA, PRA_START_EVENT, &pra_start_event_data, sizeof(PRA_START_EVENT_DATA), portMAX_DELAY);
                        if (!wait_send_ack("PRA_START_EVENT(auto_success)"))
                        {
                            round_success = false;
                            circulation = 0;
                            break;
                        }
                    }
                    if (!round_success)
                        break;

                    drain_run_queue();
                    if (!wait_window_ignoring_hits(big_first_hit_window, "first"))
                        break;
                    if (!wait_window_ignoring_hits(big_second_hit_window, "second"))
                        break;

                    snprintf(log_string, sizeof(log_string), "AutoSuccess progress %u/5", group + 1);
                    esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[RUN_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);

                    if (group < 4)
                    {
                        stop_armours(all_addr, 5);
                    }
                }

                if (!round_success)
                {
                    stop_armours(all_addr, 5);
                }
                else
                {
                    bool complete_delivery_ok = true;
                    for (uint8_t i = 0; i < 5; i++)
                    {
                        if (!send_complete_with_retry(i))
                        {
                            complete_delivery_ok = false;
                            ESP_LOGW(TAG_MAIN, "AutoSuccess COMPLETE delivery failed on armour %u", i + 1);
                            break;
                        }
                    }
                    if (!complete_delivery_ok)
                    {
                        round_success = false;
                        stop_armours(all_addr, 5);
                    }
                    else
                    {
                        vTaskDelay(5000 / portTICK_PERIOD_MS);
                        stop_armours(all_addr, 5);
                    }
                }
            }
            else
            {
                // 大符模式：5组，每组点亮2块；首击2.5s，次击可选1s；5组都成功才算成功
                for (uint8_t group = 0; group < 5; group++)
                {
                    generate_rand_sequence(rune_start_sequence, 5);
                    uint8_t active_addr[2] = {(uint8_t)(rune_start_sequence[0] - 1), (uint8_t)(rune_start_sequence[1] - 1)};
                    ESP_LOGI(TAG_MAIN, "Big Group %u targets: %u, %u", group + 1, active_addr[0] + 1, active_addr[1] + 1);
                    sprintf(log_string, "Big Group %u/5 targets: %u,%u", group + 1, active_addr[0] + 1, active_addr[1] + 1);
                    esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[RUN_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);

                    for (uint8_t addr = 0; addr < 5; addr++)
                    {
                        pra_start_event_data.address = addr;
                        pra_start_event_data.big_group_index = group + 1;
                        pra_start_event_data.big_target_armed = (addr == active_addr[0] || addr == active_addr[1]) ? 1 : 0;
                        esp_event_post_to(pr_events_loop_handle, PRA, PRA_START_EVENT, &pra_start_event_data, sizeof(PRA_START_EVENT_DATA), portMAX_DELAY);
                        if (!wait_send_ack("PRA_START_EVENT"))
                        {
                            round_success = false;
                            circulation = 0;
                            break;
                        }
                    }
                    if (!round_success)
                        break;

                    // 每组开始前清空残留命中，避免上一组尾包误判本组首击
                    drain_run_queue();
                    bool first_hit_ok = false;
                    uint8_t first_hit_addr = 0xFF;
                    TickType_t begin = xTaskGetTickCount();
                    while ((xTaskGetTickCount() - begin) < big_first_hit_window)
                    {
                        TickType_t elapsed = xTaskGetTickCount() - begin;
                        TickType_t remain = big_first_hit_window - elapsed;
                        if (!wait_queue_or_stop(run_queue, &hit_done_data, remain))
                            break;

                        if (hit_done_data.address == 10)
                        {
                            user_stop = true;
                            circulation = 0;
                            round_success = false;
                            break;
                        }
                        if (hit_done_data.address == active_addr[0] || hit_done_data.address == active_addr[1])
                        {
                            first_hit_ok = true;
                            first_hit_addr = hit_done_data.address;
                            score += hit_done_data.score;
                            ESP_LOGI(TAG_MAIN, "[HIT] Big Group %u first hit armour %u score +%u", group + 1, first_hit_addr + 1, hit_done_data.score);
                            // 命中后立即熄灭该片
                            stop_armours(&first_hit_addr, 1);
                            // 清掉首击尾包，避免次击窗口误收到同一次击打残留
                            drain_run_queue();
                            break;
                        }
                        if (hit_done_data.address == 0xFF)
                            continue;

                        ESP_LOGI(TAG_MAIN, "Big Group %u ignore non-target first hit armour %u, targets %u,%u",
                                 group + 1,
                                 hit_done_data.address + 1,
                                 active_addr[0] + 1,
                                 active_addr[1] + 1);
                        continue;
                    }
                    if (!round_success)
                        break;
                    if (!first_hit_ok)
                    {
                        ESP_LOGW(TAG_MAIN, "Big Group %u failed: no hit in 2.5s", group + 1);
                        round_success = false;
                        break;
                    }

                    // 次击可选窗口1s，不影响本组成败
                    uint8_t second_target = (first_hit_addr == active_addr[0]) ? active_addr[1] : active_addr[0];
                    begin = xTaskGetTickCount();
                    while ((xTaskGetTickCount() - begin) < big_second_hit_window)
                    {
                        TickType_t elapsed = xTaskGetTickCount() - begin;
                        TickType_t remain = big_second_hit_window - elapsed;
                        if (!wait_queue_or_stop(run_queue, &hit_done_data, remain))
                            break;
                        if (hit_done_data.address == 10)
                        {
                            user_stop = true;
                            circulation = 0;
                            round_success = false;
                            break;
                        }
                        if (hit_done_data.address == second_target)
                        {
                            score += hit_done_data.score;
                            ESP_LOGI(TAG_MAIN, "[HIT] Big Group %u second hit armour %u score +%u", group + 1, second_target + 1, hit_done_data.score);
                            // 命中后立即熄灭该片
                            stop_armours(&second_target, 1);
                            break;
                        }
                        if (hit_done_data.address == 0xFF)
                            continue;

                        ESP_LOGI(TAG_MAIN, "Big Group %u ignore non-target second hit armour %u, expected %u",
                                 group + 1,
                                 hit_done_data.address + 1,
                                 second_target + 1);
                        continue;
                    }
                    if (!round_success)
                        break;

                    sprintf(log_string, "Big progress %u/5", group + 1);
                    esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[RUN_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);

                    // 不论是否打到第二块，本组结束都切换下一组
                    // 最后一组先保留状态，交给 COMPLETE 成功等效统一收尾，避免最后一片残留靶面灯效
                    if (group < 4)
                    {
                        stop_armours(all_addr, 5);
                    }
                }

                if (!round_success)
                {
                    // 失败收敛：统一停掉5片，避免残留TARGET状态导致下一轮状态混乱
                    stop_armours(all_addr, 5);
                }
                else
                {
                    // 大符5组都成功，触发成功等效
                    bool complete_delivery_ok = true;
                    for (uint8_t i = 0; i < 5; i++)
                    {
                        if (!send_complete_with_retry(i))
                        {
                            complete_delivery_ok = false;
                            ESP_LOGW(TAG_MAIN, "Big mode COMPLETE delivery failed on armour %u", i + 1);
                            break;
                        }
                    }
                    if (!complete_delivery_ok)
                    {
                        round_success = false;
                        stop_armours(all_addr, 5);
                    }
                    else
                    {
                        vTaskDelay(5000 / portTICK_PERIOD_MS);
                        stop_armours(all_addr, 5);
                    }
                }
            }
        }

        if (!run_mode_no_motor && round_success)
        {
            update_gpa_score_history(score);
            ESP_LOGI(TAG_MAIN, "[Score: %d]PowerRune Activated Successfully", score);
            sprintf(log_string, "[Score: %d]PowerRune Activated Successfully", score);
            esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[RUN_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);
        }
        else if (!suppress_round_result_log && !user_stop)
        {
            ESP_LOGW(TAG_MAIN, "PowerRune Activation Failed");
            sprintf(log_string, "PowerRune Activation Failed");
            esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[RUN_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);
        }

        // 单次模式：本轮结束后立即停电机
        if (!run_mode_no_motor && !circulation && !motor_stopped)
        {
            if (stop_motor_reliably("PRM_STOP_EVENT(single)"))
            {
                motor_stopped = true;
            }
            else
            {
                ESP_LOGW(TAG_MAIN, "Single run end: PRM_STOP_EVENT ACK/DONE missing after retries");
            }
        }
    } while (circulation);

    ESP_LOGI(TAG_MAIN, "PowerRune Run Complete");
    sprintf(log_string, "PowerRune Run Complete");
    esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[RUN_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);

    // 恢复空闲灯效
    pra_stop(NULL, NULL, 0, NULL);
    // 发送STOP到PRM
    if (!run_mode_no_motor && !motor_stopped)
    {
        if (!stop_motor_reliably("PRM_STOP_EVENT(final)"))
        {
            ESP_LOGW(TAG_MAIN, "Final stop: PRM_STOP_EVENT ACK/DONE missing after retries");
        }
    }
    // 注销事件、删除队列、删除计时器
    if (prm_speed_handler_registered)
        esp_event_handler_unregister_with(pr_events_loop_handle, PRM, PRM_SPEED_STABLE_EVENT, prm_speed_stable_event_handler);
    if (prm_start_done_handler_registered)
        esp_event_handler_unregister_with(pr_events_loop_handle, PRM, PRM_START_DONE_EVENT, prm_start_done_event_handler);
    if (prm_unlock_done_handler_registered)
        esp_event_handler_unregister_with(pr_events_loop_handle, PRM, PRM_UNLOCK_DONE_EVENT, prm_unlock_done_event_handler);
    if (prm_stop_done_handler_registered)
        esp_event_handler_unregister_with(pr_events_loop_handle, PRM, PRM_STOP_DONE_EVENT, prm_stop_done_event_handler);
    if (pra_hit_handler_registered)
        esp_event_handler_unregister_with(pr_events_loop_handle, PRA, PRA_HIT_EVENT, hit_event_handler);
    if (run_queue != NULL)
    {
        vQueueDelete(run_queue);
        run_queue = NULL;
    }
    if (hit_timer != NULL)
    {
        xTimerDelete(hit_timer, portMAX_DELAY);
        hit_timer = NULL;
    }
    if (motor_done_sem != NULL)
    {
        vSemaphoreDelete(motor_done_sem);
        motor_done_sem = NULL;
    }
    if (motor_start_done_queue != NULL)
    {
        vQueueDelete(motor_start_done_queue);
    }
    if (motor_unlock_done_queue != NULL)
    {
        vQueueDelete(motor_unlock_done_queue);
    }
    if (motor_stop_done_queue != NULL)
    {
        vQueueDelete(motor_stop_done_queue);
    }

    bool restart_next = g_run_restart_requested;
    g_run_restart_requested = false;
    g_run_stop_requested = false;
    if (restart_next)
    {
        ESP_LOGI(TAG_MAIN, "Queued RUN detected, restarting run_task");
        xTaskCreate((TaskFunction_t)run_task, "run_task", 4096, NULL, 10, NULL);
    }
    vTaskDelete(NULL);
}

void ota_task(void *pvParameter)
{
    OTA_BEGIN_EVENT_DATA ota_begin_event_data;
    // 队列接收，等待所有设备OTA完成
    OTA_COMPLETE_EVENT_DATA ota_complete_event_data;
    // 发送indicator日志
    char log_string[100] = "Starting OTA Operation";

    esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[OTA_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);

    // 发送STOP到所有设备
    for (size_t i = 0; i < 5; i++) // TODO: 把这里改成已连接设备数
    {
        PRA_STOP_EVENT_DATA pra_stop_event_data;
        pra_stop_event_data.address = i;
        esp_event_post_to(pr_events_loop_handle, PRA, PRA_STOP_EVENT, &pra_stop_event_data, sizeof(PRA_STOP_EVENT_DATA), portMAX_DELAY);
        // 等待ACK
        xEventGroupWaitBits(ESPNowProtocol::send_state, ESPNowProtocol::SEND_ACK_OK_BIT, pdTRUE, pdTRUE, portMAX_DELAY);
    }
    pra_stop(NULL, NULL, 0, NULL);
    PRM_STOP_EVENT_DATA prm_stop_event_data;
    prm_stop_event_data.address = MOTOR;
    esp_event_post_to(pr_events_loop_handle, PRM, PRM_STOP_EVENT, &prm_stop_event_data, sizeof(PRM_STOP_EVENT_DATA), portMAX_DELAY);
    // 等待ACK
    xEventGroupWaitBits(ESPNowProtocol::send_state, ESPNowProtocol::SEND_ACK_OK_BIT, pdTRUE, pdTRUE, portMAX_DELAY);

    // 置位队列listening bit
    // 生成队列
    Firmware::ota_complete_queue = xQueueCreate(5, sizeof(OTA_COMPLETE_EVENT_DATA));
    assert(Firmware::ota_complete_queue != NULL);
    xEventGroupSetBits(Firmware::ota_event_group, Firmware::OTA_COMPLETE_LISTENING_BIT);
    // 命令各个设备开始OTA，先暂停ESP_NOW收发，然后重新启动ESP_NOW收发
    for (size_t i = 0; i < 6; i++) // TODO: 把这里改成已连接设备数
    {
        // 字符串打印到log_string
        sprintf(log_string, "Triggering OTA for device %d", i);
        esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[OTA_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);
        ota_begin_event_data.address = i;
        esp_event_post_to(pr_events_loop_handle, PRC, OTA_BEGIN_EVENT, &ota_begin_event_data, sizeof(OTA_BEGIN_EVENT_DATA), portMAX_DELAY);
        // 等待ACK
        xEventGroupWaitBits(ESPNowProtocol::send_state, ESPNowProtocol::SEND_ACK_OK_BIT, pdTRUE, pdTRUE, portMAX_DELAY);
    }
    for (size_t i = 0; i < 6; i++) // TODO: 把这里改成已连接设备数
    {
        xQueueReceive(Firmware::ota_complete_queue, &ota_complete_event_data, portMAX_DELAY);
        esp_log_buffer_hex(TAG_MAIN, &ota_complete_event_data, sizeof(OTA_COMPLETE_EVENT_DATA));
        if (ota_complete_event_data.status != ESP_OK)
        {
            if (ota_complete_event_data.status == ESP_ERR_NOT_SUPPORTED)
                sprintf(log_string, "OTA for device %i skipped", ota_complete_event_data.address);
            else
                sprintf(log_string, "OTA for device %i failed [%s]", ota_complete_event_data.address, esp_err_to_name(ota_complete_event_data.status));

            esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[OTA_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);
        }
        else
        {
            sprintf(log_string, "OTA for device %i complete", ota_complete_event_data.address);
            esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[OTA_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);
        }
    }
    // 更新自己
    ota_begin_event_data.address = 0x06;
    set_logo_off_mode();
    sprintf(log_string, "Starting OTA for PowerRune Server");
    esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[OTA_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);

    esp_event_post_to(pr_events_loop_handle, PRC, OTA_BEGIN_EVENT, &ota_begin_event_data, sizeof(OTA_BEGIN_EVENT_DATA), portMAX_DELAY);
    // 等待队列接收
    xQueueReceive(Firmware::ota_complete_queue, &ota_complete_event_data, portMAX_DELAY);
    esp_log_buffer_hex(TAG_MAIN, &ota_complete_event_data, sizeof(OTA_COMPLETE_EVENT_DATA));
    if (ota_complete_event_data.status != ESP_OK)
    {
        if (ota_complete_event_data.status == ESP_ERR_NOT_SUPPORTED)
            sprintf(log_string, "OTA for server skipped");
        else
            sprintf(log_string, "OTA for server failed [%s]", esp_err_to_name(ota_complete_event_data.status));

        esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[OTA_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);
    }
    else
    {
        sprintf(log_string, "OTA for server complete, ready for restart");
        esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[OTA_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);
    }
    led->set_mode(LED_MODE_FADE, 0);
    // 更新完成，准备重启
    if (ota_complete_event_data.status == ESP_OK)
    {
        vTaskDelay(5000 / portTICK_PERIOD_MS);
        esp_restart();
    }
    pra_stop(NULL, NULL, 0, NULL);
    sprintf(log_string, "OTA operation complete");
    esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, ops_handle_table[OTA_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);
    xEventGroupClearBits(Firmware::ota_event_group, Firmware::OTA_COMPLETE_LISTENING_BIT);
    vQueueDelete(Firmware::ota_complete_queue);
    vTaskDelete(NULL);
}

void config_task(void *pvParameter)
{
    CONFIG_EVENT_DATA config_event_data;
    memcpy(&config_event_data.config_common_info, config->get_config_common_info_pt(), sizeof(PowerRune_Common_config_info_t));
    memcpy(&config_event_data.config_motor_info, config->get_config_motor_info_pt(), sizeof(PowerRune_Motor_config_info_t));
    memcpy(&config_event_data.config_armour_info, config->get_config_armour_info_pt(ARMOUR1), sizeof(PowerRune_Armour_config_info_t));
    char log_string[100];
    if (pvParameter == NULL || *(uint8_t *)pvParameter == ARMOUR1)
    {
        // 发送notify，统一发送到URL_VAL
        sprintf(log_string, "Sending configuration to armour devices");
        esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, spp_handle_table[URL_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);
        // 发送给所有装甲板设备
        for (uint8_t i = 0; i < 5; i++)
        {
            config_event_data.config_armour_info.armour_id = i + 1;
            config_event_data.address = i;
            esp_event_post_to(pr_events_loop_handle, PRC, CONFIG_EVENT, &config_event_data, sizeof(CONFIG_EVENT_DATA), portMAX_DELAY);
            // 等待ACK
            xEventGroupWaitBits(ESPNowProtocol::send_state, ESPNowProtocol::SEND_ACK_OK_BIT, pdTRUE, pdTRUE, portMAX_DELAY);
        }
        sprintf(log_string, "Configuration sent to all armour devices");
        esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, spp_handle_table[URL_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);
    }
    if (pvParameter == NULL || *(uint8_t *)pvParameter == MOTOR)
    {
        // 发送给电机设备
        sprintf(log_string, "Sending configuration to motor device");
        esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, spp_handle_table[URL_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);
        config_event_data.address = MOTOR;
        esp_event_post_to(pr_events_loop_handle, PRC, CONFIG_EVENT, &config_event_data, sizeof(CONFIG_EVENT_DATA), portMAX_DELAY);
        // 等待ACK
        xEventGroupWaitBits(ESPNowProtocol::send_state, ESPNowProtocol::SEND_ACK_OK_BIT, pdTRUE, pdTRUE, portMAX_DELAY);
        sprintf(log_string, "Configuration sent to motor device");
        esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, spp_handle_table[URL_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);
    }
    vTaskDelete(NULL);
}

void reset_armour_id_task(void *pvParameter)
{
    char log_string[] = "Resetting Armour IDs";
    esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, spp_handle_table[ARMOUR_ID_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);
    ESPNowProtocol::reset_armour_id();
    sprintf(log_string, "Armour IDs reset");
    esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, spp_handle_table[ARMOUR_ID_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);
    vTaskDelete(NULL);
}

// 7x7蛇形映射：第1行左->右，第2行右->左，依次类推
static inline uint16_t map_logo_logical_to_physical(uint16_t logical_index)
{
    const uint16_t cols = 7;
    const uint16_t total = 49;
    if (logical_index >= total)
        return logical_index;
    uint16_t row = logical_index / cols;
    uint16_t col = logical_index % cols;
    if ((row & 0x01) == 0)
        return row * cols + col;
    return row * cols + (cols - 1 - col);
}

static inline void hsv_to_rgb_u8(uint8_t h, uint8_t s, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    if (s == 0)
    {
        *r = v;
        *g = v;
        *b = v;
        return;
    }

    uint8_t region = h / 43;               // 256 / 6 ~= 43
    uint8_t remainder = (h - region * 43) * 6;
    uint8_t p = (uint8_t)((uint16_t)v * (255 - s) / 255);
    uint8_t q = (uint8_t)((uint16_t)v * (255 - (uint16_t)s * remainder / 255) / 255);
    uint8_t t = (uint8_t)((uint16_t)v * (255 - (uint16_t)s * (255 - remainder) / 255) / 255);

    switch (region)
    {
    case 0:
        *r = v;
        *g = t;
        *b = p;
        break;
    case 1:
        *r = q;
        *g = v;
        *b = p;
        break;
    case 2:
        *r = p;
        *g = v;
        *b = t;
        break;
    case 3:
        *r = p;
        *g = q;
        *b = v;
        break;
    case 4:
        *r = t;
        *g = p;
        *b = v;
        break;
    default:
        *r = v;
        *g = p;
        *b = q;
        break;
    }
}

void led_animation_task(void *pvParameter)
{
    const uint16_t logo_pixel_count = 49;
    uint16_t mono_breath_phase = 0; // 0~511
    uint8_t flow_phase = 0;
    uint8_t base_hue = 0;
    const PowerRune_Rlogo_config_info_t *config_rlogo = config->get_config_info_pt();
    while (1)
    {
        config_rlogo = config->get_config_info_pt();
        const uint8_t animation_mode = g_logo_animation_mode;
        const uint8_t animation_color = sanitize_logo_color(g_logo_animation_color);
        const uint8_t brightness = config_rlogo->brightness;
        const uint8_t gradient_v = (brightness < 32) ? 32 : brightness;

        auto fill_logo_color = [&](uint8_t r, uint8_t g, uint8_t b) {
            for (uint16_t logical_index = 0; logical_index < logo_pixel_count; logical_index++)
            {
                uint16_t physical_index = map_logo_logical_to_physical(logical_index);
                led_strip->set_color_index(physical_index, r, g, b);
            }
        };

        switch (animation_mode)
        {
        case LOGO_ANIM_MONO_BREATH:
        {
            uint16_t tri = (mono_breath_phase < 256) ? mono_breath_phase : (511 - mono_breath_phase); // 0~255
            uint8_t breath_level = (uint8_t)((tri * 191) / 255 + 64);                                  // 25%~100%
            uint8_t value = (uint8_t)((uint16_t)brightness * breath_level / 255);
            if (animation_color == PR_BLUE)
                fill_logo_color(0, 0, value);
            else
                fill_logo_color(value, 0, 0);
            mono_breath_phase = (mono_breath_phase + 4) & 0x01FF;
            break;
        }
        case LOGO_ANIM_SOLID:
        {
            if (animation_color == PR_BLUE)
                fill_logo_color(0, 0, brightness);
            else
                fill_logo_color(brightness, 0, 0);
            break;
        }
        case LOGO_ANIM_OFF:
        {
            fill_logo_color(0, 0, 0);
            break;
        }
        case LOGO_ANIM_IDLE_FLOW_GRADIENT:
        default:
        {
            for (uint16_t logical_index = 0; logical_index < logo_pixel_count; logical_index++)
            {
                uint8_t pixel_hue = (uint8_t)(base_hue + logical_index * 6 + flow_phase);
                uint8_t r = 0, g = 0, b = 0;
                hsv_to_rgb_u8(pixel_hue, 255, gradient_v, &r, &g, &b);
                uint16_t physical_index = map_logo_logical_to_physical(logical_index);
                led_strip->set_color_index(physical_index, r, g, b);
            }
            flow_phase += 2;
            base_hue += 1;
            break;
        }
        }

        led_strip->refresh();

        // 接受任务通知仅重置相位，避免清屏闪断
        if (ulTaskNotifyTake(pdTRUE, 0))
        {
            mono_breath_phase = 0;
            flow_phase = 0;
            base_hue = 0;
        }

        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
}

// GATTS最终的回调函数
void gatts_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    esp_ble_gatts_cb_param_t *p_data = (esp_ble_gatts_cb_param_t *)param;
    uint8_t res;

    ESP_LOGI(GATTS_TABLE_TAG, "event = %x\n", event);
    switch (event)
    {
    case ESP_GATTS_REG_EVT:
        // 注册事件
        esp_ble_gap_set_device_name(DEVICE_NAME);
        esp_ble_gap_config_adv_data_raw((uint8_t *)spp_adv_data, sizeof(spp_adv_data));

        // 创建属性表
        esp_ble_gatts_create_attr_tab(spp_gatt_db, gatts_if, SPP_IDX_NB, SPP_SVC_INST_ID);
        ESP_LOGD(TAG_BLE, "注册spp属性表结束\n");
        esp_ble_gatts_create_attr_tab(ops_gatt_db, gatts_if, OPS_IDX_NB, OPS_SVC_INST_ID);
        ESP_LOGD(TAG_BLE, "注册ops属性表结束\n");
        break;
    case ESP_GATTS_READ_EVT:
    {
        // read事件
        res = find_char_and_desr_index(p_data->read.handle);
        switch (res)
        {
        case URL_VAL:
        {
            // URL_read事件
            // 回复READ
            ESP_LOGD(TAG_BLE, "URL_read事件\n");
            break;
        }
        case URL_CFG:
            // URL_cfg
            ESP_LOGD(TAG_BLE, "URL_cfg(read)\n");
            break;
        case SSID_VAL:
        {
            // SSID_read事件
            ESP_LOGD(TAG_BLE, "SSID_read事件\n");

            break;
        }
        case SSID_CFG:
            // SSID_cfg
            ESP_LOGD(TAG_BLE, "SSID_cfg(read)\n");
            break;
        case Wifi_VAL:
        {
            // Wifi_read事件
            ESP_LOGD(TAG_BLE, "Wifi_read事件\n");

            break;
        }
        case Wifi_CFG:
            // Wifi_cfg
            ESP_LOGD(TAG_BLE, "Wifi_cfg(read)\n");
            break;
        case AOTA_VAL:
        {
            // AOTA_read事件
            ESP_LOGD(TAG_BLE, "AOTA_read事件\n");

            break;
        }
        case AOTA_CFG:
            // AOTA_cfg
            ESP_LOGD(TAG_BLE, "AOTA_cfg(read)\n");
            break;
        case LIT_VAL:
        {
            // LIT_read事件，回复brightness
            ESP_LOGD(TAG_BLE, "LIT_read事件\n");

            break;
        }
        case LIT_CFG:
            // LIT_cfg
            ESP_LOGD(TAG_BLE, "LIT_cfg(read)\n");
            break;
        case ARM_LIT_VAL:
        {
            // ARM_LIT_read事件
            ESP_LOGD(TAG_BLE, "ARM_LIT_read事件\n");

            break;
        }
        case ARM_LIT_CFG:
            // ARM_LIT_cfg
            ESP_LOGD(TAG_BLE, "ARM_LIT_cfg(read)\n");
            break;
        case R_LIT_VAL:
        {
            // R_LIT_read事件
            ESP_LOGD(TAG_BLE, "R_LIT_read事件\n");

            break;
        }
        case R_LIT_CFG:
            // R_LIT_cfg
            ESP_LOGD(TAG_BLE, "R_LIT_cfg(read)\n");
            break;
        case MATRIX_LIT_VAL:
        {
            // MATRIX_LIT_read事件
            ESP_LOGD(TAG_BLE, "MATRIX_LIT_read事件\n");

            break;
        }
        case MATRIX_LIT_CFG:
            // MATRIX_LIT_cfg
            ESP_LOGD(TAG_BLE, "MATRIX_LIT_cfg(read)\n");
            break;
        case PID_VAL:
        {
            // PID_read事件，发送顺序：P, I, D, I_MAX, D_MAX, OUT_MAX
            ESP_LOGD(TAG_BLE, "PID_read事件\n");

            break;
        }
        case PID_CFG:
            // PID_cfg
            ESP_LOGD(TAG_BLE, "PID_cfg(read)\n");
            break;
        case ARMOUR_ID_CFG:
            // ARMOUR_ID_cfg
            ESP_LOGD(TAG_BLE, "ARMOUR_ID_cfg(read)\n");
            break;
        case (uint8_t)GPA_VAL + (uint8_t)SPP_IDX_NB:
        {
            // GPA_read事件
            ESP_LOGD(TAG_BLE, "GPA_read事件\n");

            ESP_LOGD(TAG_BLE, "GPA_read事件结束\n");
            break;
        }
        default:
            ESP_LOGD(TAG_BLE, "未知read事件\n");
        }
        break;
    }
    case ESP_GATTS_WRITE_EVT:
    {
        // write事件
        res = find_char_and_desr_index(p_data->write.handle);
        ESP_LOGD(TAG_BLE, "write事件  pdata handle: %d\n", res);
        if (p_data->write.is_prep == false)
        {
            switch (res)
            {
            case URL_VAL:
                // URL_write事件
                ESP_LOGD(TAG_BLE, "URL_write事件\n");
                esp_ble_gatts_set_attr_value(p_data->write.handle, p_data->write.len, p_data->write.value);
                break;
            case URL_CFG:
                // URL_cfg
                ESP_LOGD(TAG_BLE, "URL_cfg(write)\n");
                break;
            case SSID_VAL:
                // SSID_write事件
                ESP_LOGD(TAG_BLE, "SSID_write事件\n");
                esp_ble_gatts_set_attr_value(p_data->write.handle, p_data->write.len, p_data->write.value);
                break;
            case SSID_CFG:
                // SSID_cfg
                ESP_LOGD(TAG_BLE, "SSID_cfg(write)\n");
                break;
            case Wifi_VAL:
                // Wifi_write事件
                ESP_LOGD(TAG_BLE, "Wifi_write事件\n");
                esp_ble_gatts_set_attr_value(p_data->write.handle, p_data->write.len, p_data->write.value);
                break;
            case Wifi_CFG:
                // Wifi_cfg
                ESP_LOGD(TAG_BLE, "Wifi_cfg(write)\n");
                break;
            case AOTA_VAL:
                // AOTA_write事件
                ESP_LOGD(TAG_BLE, "AOTA_write事件\n");
                esp_ble_gatts_set_attr_value(p_data->write.handle, p_data->write.len, p_data->write.value);
                break;
            case AOTA_CFG:
                // AOTA_cfg
                ESP_LOGD(TAG_BLE, "AOTA_cfg(write)\n");
                break;
            case LIT_VAL:
                // LIT_write事件
                ESP_LOGD(TAG_BLE, "LIT_write事件\n");
                esp_ble_gatts_set_attr_value(p_data->write.handle, p_data->write.len, p_data->write.value);
                break;
            case LIT_CFG:
                // LIT_cfg
                ESP_LOGD(TAG_BLE, "LIT_cfg(write)\n");
                break;
            case ARM_LIT_VAL:
                // ARM_LIT_write事件
                ESP_LOGD(TAG_BLE, "ARM_LIT_write事件\n");
                esp_ble_gatts_set_attr_value(p_data->write.handle, p_data->write.len, p_data->write.value);
                break;
            case ARM_LIT_CFG:
                // ARM_LIT_cfg
                ESP_LOGD(TAG_BLE, "ARM_LIT_cfg(write)\n");
                break;
            case R_LIT_VAL:
                // R_LIT_write事件
                ESP_LOGD(TAG_BLE, "R_LIT_write事件\n");
                esp_ble_gatts_set_attr_value(p_data->write.handle, p_data->write.len, p_data->write.value);
                break;
            case R_LIT_CFG:
                // R_LIT_cfg
                ESP_LOGD(TAG_BLE, "R_LIT_cfg(write)\n");
                break;
            case MATRIX_LIT_VAL:
                // MATRIX_LIT_write事件
                ESP_LOGD(TAG_BLE, "MATRIX_LIT_write事件\n");
                esp_ble_gatts_set_attr_value(p_data->write.handle, p_data->write.len, p_data->write.value);
                break;
            case MATRIX_LIT_CFG:
                // MATRIX_LIT_cfg
                ESP_LOGD(TAG_BLE, "MATRIX_LIT_cfg(write)\n");
                break;
            case PID_VAL:
                // PID_write事件
                ESP_LOGD(TAG_BLE, "PID_write事件\n");
                esp_ble_gatts_set_attr_value(p_data->write.handle, p_data->write.len, p_data->write.value);
                break;
            case PID_CFG:
                // PID_cfg
                ESP_LOGD(TAG_BLE, "PID_cfg(write)\n");
                break;
            case ARMOUR_ID_VAL:
                // ARMOUR_ID_write事件
                ESP_LOGD(TAG_BLE, "ARMOUR_ID_write事件\n");
                esp_ble_gatts_set_attr_value(p_data->write.handle, p_data->write.len, p_data->write.value);
                break;
            case ARMOUR_ID_CFG:
                // ARMOUR_ID_cfg
                ESP_LOGD(TAG_BLE, "ARMOUR_ID_cfg(write)\n");
                break;
            case (uint8_t)RUN_VAL + (uint8_t)SPP_IDX_NB:
                // RUN_write事件
                ESP_LOGD(TAG_BLE, "RUN_write事件\n");
                esp_ble_gatts_set_attr_value(p_data->write.handle, p_data->write.len, p_data->write.value);
                break;
            case (uint8_t)RUN_CFG + (uint8_t)SPP_IDX_NB:
                // RUN_cfg
                ESP_LOGD(TAG_BLE, "RUN_cfg(write)\n");
                break;
            case (uint8_t)UNLK_VAL + (uint8_t)SPP_IDX_NB:
                // UNLK_write事件
                ESP_LOGD(TAG_BLE, "UNLK_write事件\n");
                esp_ble_gatts_set_attr_value(p_data->write.handle, p_data->write.len, p_data->write.value);
                break;
            case (uint8_t)UNLK_CFG + (uint8_t)SPP_IDX_NB:
                // UNLK_cfg
                ESP_LOGD(TAG_BLE, "UNLK_cfg(write)\n");
                break;
            case (uint8_t)STOP_VAL + (uint8_t)SPP_IDX_NB:
                // STOP_write事件
                ESP_LOGD(TAG_BLE, "STOP_write事件\n");
                esp_ble_gatts_set_attr_value(p_data->write.handle, p_data->write.len, p_data->write.value);
                break;
            case (uint8_t)STOP_CFG + (uint8_t)SPP_IDX_NB:
                // STOP_cfg
                ESP_LOGD(TAG_BLE, "STOP_cfg(write)\n");
                break;
            case (uint8_t)OTA_VAL + (uint8_t)SPP_IDX_NB:
                // OTA_write事件
                ESP_LOGD(TAG_BLE, "OTA_write事件\n");
                esp_ble_gatts_set_attr_value(p_data->write.handle, p_data->write.len, p_data->write.value);
                break;
            case (uint8_t)OTA_CFG + (uint8_t)SPP_IDX_NB:
                // OTA_cfg
                ESP_LOGD(TAG_BLE, "OTA_cfg(write)\n");
                break;
            default:
                ESP_LOGD(TAG_BLE, "未知write事件\n");
            }
        }
        else if ((p_data->write.is_prep == true))
        {
            ESP_LOGI(GATTS_TABLE_TAG, "ESP_GATTS_PREP_WRITE_EVT : handle = %d\n", res);
            store_wr_buffer(p_data);
        }
        break;
    }
    case ESP_GATTS_MTU_EVT:
        spp_mtu_size = p_data->mtu.mtu;
        break;
    case ESP_GATTS_CONF_EVT:
        // 经过esp_ble_gatts_send_indicate会到此处
        ESP_LOGD(TAG_BLE, "经过send_indicate后\n");
        break;
    case ESP_GATTS_UNREG_EVT:
        break;
    case ESP_GATTS_DELETE_EVT:
        break;
    case ESP_GATTS_START_EVT:
        break;
    case ESP_GATTS_STOP_EVT:
        break;
    case ESP_GATTS_CONNECT_EVT:
        spp_conn_id = p_data->connect.conn_id;
        spp_gatts_if = gatts_if;
        is_connected = true;
        memcpy(&spp_remote_bda, &p_data->connect.remote_bda, sizeof(esp_bd_addr_t));
        break;
    case ESP_GATTS_DISCONNECT_EVT:
        is_connected = false;
        enable_data_ntf = false;
        esp_ble_gap_start_advertising(&spp_adv_params);
        break;
    case ESP_GATTS_OPEN_EVT:
        break;
    case ESP_GATTS_CANCEL_OPEN_EVT:
        break;
    case ESP_GATTS_CLOSE_EVT:
        break;
    case ESP_GATTS_LISTEN_EVT:
        break;
    case ESP_GATTS_CONGEST_EVT:
        break;
    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
    {
        if (param->add_attr_tab.status != ESP_GATT_OK)
        {
            ESP_LOGE(GATTS_TABLE_TAG, "Create attribute table failed, error code=0x%x", param->add_attr_tab.status);
        }
        // 创建属性表后启动服务
        else if (param->add_attr_tab.svc_inst_id == 0)
        {
            memcpy(spp_handle_table, param->add_attr_tab.handles, sizeof(spp_handle_table));
            esp_ble_gatts_start_service(spp_handle_table[SPP_IDX_SVC]);

            ESP_LOGD(TAG_BLE, "spp_handle_table[0] = %d\n", spp_handle_table[0]);
        }
        else if (param->add_attr_tab.svc_inst_id == 1)
        {
            memcpy(ops_handle_table, param->add_attr_tab.handles, sizeof(ops_handle_table));
            esp_ble_gatts_start_service(ops_handle_table[OPS_IDX_SVC]);

            ESP_LOGD(TAG_BLE, "ops_handle_table[0] = %d\n", ops_handle_table[0]);
        }
        break;
    }
    case ESP_GATTS_SET_ATTR_VAL_EVT:
    {
        // 当设置属性表完成时，到这里
        res = find_char_and_desr_index(param->set_attr_val.attr_handle);
        switch (res)
        {
        case URL_VAL:
        {
            // Update URL 设置
            // 获取特征值
            uint16_t len;
            const uint8_t *value;
            esp_ble_gatts_get_attr_value(spp_handle_table[URL_VAL], &len, &value);
            strcpy(config->get_config_common_info_pt()->URL, (char *)value);
            // 启动config_task
            xTaskCreate(config_task, "config_task", 4096, NULL, 5, NULL);
            break;
        }
        case SSID_VAL:
        {
            // SSID
            uint16_t len;
            const uint8_t *value;
            esp_ble_gatts_get_attr_value(spp_handle_table[SSID_VAL], &len, &value);
            strcpy(config->get_config_common_info_pt()->SSID, (char *)value);
            // 启动config_task
            xTaskCreate(config_task, "config_task", 4096, NULL, 5, NULL);
            break;
        }
        case Wifi_VAL:
        {
            // Wifi
            uint16_t len;
            const uint8_t *value;
            esp_ble_gatts_get_attr_value(spp_handle_table[Wifi_VAL], &len, &value);
            strcpy(config->get_config_common_info_pt()->SSID_pwd, (char *)value);
            // 启动config_task
            xTaskCreate(config_task, "config_task", 4096, NULL, 5, NULL);
            break;
        }
        case AOTA_VAL:
        {
            // AOTA
            uint16_t len;
            const uint8_t *value;
            esp_ble_gatts_get_attr_value(spp_handle_table[AOTA_VAL], &len, &value);
            config->get_config_common_info_pt()->auto_update = *value ? 1 : 0;
            config->save();
            // 启动config_task
            xTaskCreate(config_task, "config_task", 4096, NULL, 5, NULL);
            break;
        }
        case LIT_VAL:
        {
            // LIT
            uint16_t len;
            const uint8_t *value;
            esp_ble_gatts_get_attr_value(spp_handle_table[LIT_VAL], &len, &value);
            config->get_config_armour_info_pt(ARMOUR1)->brightness = *value;
            // 启动config_task
            static uint8_t config_armour_only = ARMOUR1;
            xTaskCreate(config_task, "config_task", 4096, &config_armour_only, 5, NULL);
            break;
        }
        case ARM_LIT_VAL:
        {
            // ARM_LIT
            uint16_t len;
            const uint8_t *value;
            esp_ble_gatts_get_attr_value(spp_handle_table[ARM_LIT_VAL], &len, &value);
            config->get_config_armour_info_pt(ARMOUR1)->brightness_proportion_edge = *value;
            // 启动config_task
            static uint8_t config_armour_only = ARMOUR1;
            xTaskCreate(config_task, "config_task", 4096, &config_armour_only, 5, NULL);
            break;
        }
        case R_LIT_VAL:
        {
            // R_LIT
            uint16_t len;
            const uint8_t *value;
            esp_ble_gatts_get_attr_value(spp_handle_table[R_LIT_VAL], &len, &value);
            config->get_config_info_pt()->brightness = *value;
            config->save();
            char log_string[] = "Configuration sent to RLogo device";
            esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, spp_handle_table[URL_VAL], strlen(log_string) + 1, (uint8_t *)log_string, false);
            break;
        }
        case MATRIX_LIT_VAL:
            // MATRIX_LIT
            uint16_t len;
            const uint8_t *value;
            esp_ble_gatts_get_attr_value(spp_handle_table[MATRIX_LIT_VAL], &len, &value);
            config->get_config_armour_info_pt(ARMOUR1)->brightness_proportion_matrix = *value;
            // 启动config_task
            xTaskCreate(config_task, "config_task", 4096, NULL, 5, NULL);
            break;
        case PID_VAL:
        {
            static uint8_t config_motor_only = MOTOR;
            uint16_t len;
            const uint8_t *value;
            esp_ble_gatts_get_attr_value(spp_handle_table[PID_VAL], &len, &value);
            // PID
            ESP_LOGI(TAG_BLE, "PID_val: %f, %f, %f, %f, %f, %f", *(float *)(value + 0), *(float *)(value + 1 * sizeof(float)), *(float *)(value + 2 * sizeof(float)), *(float *)(value + 3 * sizeof(float)), *(float *)(value + 4 * sizeof(float)), *(float *)(value + 5 * sizeof(float)));
            memcpy(&config->get_config_motor_info_pt()->kp, value + 0, sizeof(float));
            memcpy(&config->get_config_motor_info_pt()->ki, value + 1 * sizeof(float), sizeof(float));
            memcpy(&config->get_config_motor_info_pt()->kd, value + 2 * sizeof(float), sizeof(float));
            memcpy(&config->get_config_motor_info_pt()->i_max, value + 3 * sizeof(float), sizeof(float));
            memcpy(&config->get_config_motor_info_pt()->d_max, value + 4 * sizeof(float), sizeof(float));
            memcpy(&config->get_config_motor_info_pt()->out_max, value + 5 * sizeof(float), sizeof(float));

            // 启动config_task
            xTaskCreate(config_task, "config_task", 4096, &config_motor_only, 5, NULL);
            break;
        }
        case ARMOUR_ID_VAL:
            // ARMOUR_ID
            xTaskCreate(reset_armour_id_task, "reset_armour_id_task", 4096, NULL, 5, NULL);
            break;
        case (uint8_t)RUN_VAL + (uint8_t)SPP_IDX_NB:
            // RUN
        {
            TaskHandle_t run_h = xTaskGetHandle("run_task");
            TaskHandle_t stop_h = xTaskGetHandle("stop_task");
            if (run_h != NULL && eTaskGetState(run_h) != eDeleted)
            {
                g_run_restart_requested = true;
                ESP_LOGW(TAG_MAIN, "run_task already running, queue RUN trigger");
            }
            else if (stop_h != NULL && eTaskGetState(stop_h) != eDeleted)
            {
                g_run_restart_requested = true;
                ESP_LOGW(TAG_MAIN, "stop_task running, queue RUN trigger");
            }
            else
            {
                g_run_restart_requested = false;
                xTaskCreate((TaskFunction_t)run_task, "run_task", 4096, NULL, 10, NULL);
            }
            break;
        }
        case (uint8_t)UNLK_VAL + (uint8_t)SPP_IDX_NB:
            // UNLK
        {
            TaskHandle_t h = xTaskGetHandle("unlock_task");
            if (h != NULL && eTaskGetState(h) != eDeleted)
            {
                ESP_LOGW(TAG_MAIN, "unlock_task already running, ignore UNLK trigger");
            }
            else
            {
                xTaskCreate((TaskFunction_t)unlock_task, "unlock_task", 4096, NULL, 10, NULL);
            }
            break;
        }
        case (uint8_t)STOP_VAL + (uint8_t)SPP_IDX_NB:
        {
            TaskHandle_t h = xTaskGetHandle("stop_task");
            if (h != NULL && eTaskGetState(h) != eDeleted)
            {
                ESP_LOGW(TAG_MAIN, "stop_task already running, ignore STOP trigger");
            }
            else
            {
                xTaskCreate((TaskFunction_t)stop_task, "stop_task", 4096, NULL, 10, NULL);
            }
            break;
        }
        case (uint8_t)OTA_VAL + (uint8_t)SPP_IDX_NB:
            // OTA
            xTaskCreate((TaskFunction_t)ota_task, "ota_task", 8192, NULL, 10, NULL);
            break;
        }
        break;
    }
    default:
        break;
    }
}

extern "C" void app_main(void)
{
    // LED and LED Strip init
    led = new LED(GPIO_NUM_2);
    led_strip = new LED_Strip(GPIO_NUM_8, 49);

    // 启动事件循环
    esp_event_loop_args_t loop_args = {
        .queue_size = 10,
        .task_name = "pr_events_loop",
        .task_priority = 2,
        .task_stack_size = 4096,
    };
    ESP_ERROR_CHECK(esp_event_loop_create(&loop_args, &pr_events_loop_handle));

    // Firmware init
    Firmware firmware;

    // ESP-NOW init
    espnow_protocol = new ESPNowProtocol();

    // 注册大符通讯协议事件
    // 发送事件
    ESP_ERROR_CHECK(esp_event_handler_register_with(pr_events_loop_handle, PRC, OTA_BEGIN_EVENT, ESPNowProtocol::tx_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register_with(pr_events_loop_handle, PRC, CONFIG_EVENT, ESPNowProtocol::tx_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register_with(pr_events_loop_handle, PRA, PRA_START_EVENT, ESPNowProtocol::tx_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register_with(pr_events_loop_handle, PRA, PRA_STOP_EVENT, ESPNowProtocol::tx_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register_with(pr_events_loop_handle, PRA, PRA_COMPLETE_EVENT, ESPNowProtocol::tx_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register_with(pr_events_loop_handle, PRM, PRM_START_EVENT, ESPNowProtocol::tx_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register_with(pr_events_loop_handle, PRM, PRM_UNLOCK_EVENT, ESPNowProtocol::tx_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register_with(pr_events_loop_handle, PRM, PRM_STOP_EVENT, ESPNowProtocol::tx_event_handler, NULL));

    ESP_ERROR_CHECK(esp_event_handler_register_with(pr_events_loop_handle, PRC, OTA_BEGIN_EVENT, Firmware::global_pr_event_handler, NULL));

    // BLE 特征值设置
    memcpy(url_val, config->get_config_common_info_pt()->URL, 100);
    memcpy(ssid_val, config->get_config_common_info_pt()->SSID, 20);
    memcpy(wifi_val, config->get_config_common_info_pt()->SSID_pwd, 20);
    aota_val[0] = config->get_config_common_info_pt()->auto_update;
    lit_val[0] = config->get_config_armour_info_pt(ARMOUR1)->brightness;
    arm_lit_val[0] = config->get_config_armour_info_pt(ARMOUR1)->brightness_proportion_edge;
    r_lit_val[0] = config->get_config_info_pt()->brightness;
    matrix_lit_val[0] = config->get_config_armour_info_pt(ARMOUR1)->brightness_proportion_matrix;
    memcpy(pid_val, &(config->get_config_motor_info_pt()->kp), sizeof(float));
    memcpy(pid_val + 1 * sizeof(float), &(config->get_config_motor_info_pt()->ki), sizeof(float));
    memcpy(pid_val + 2 * sizeof(float), &(config->get_config_motor_info_pt()->kd), sizeof(float));
    memcpy(pid_val + 3 * sizeof(float), &(config->get_config_motor_info_pt()->i_max), sizeof(float));
    memcpy(pid_val + 4 * sizeof(float), &(config->get_config_motor_info_pt()->d_max), sizeof(float));
    memcpy(pid_val + 5 * sizeof(float), &(config->get_config_motor_info_pt()->out_max), sizeof(float));

    // BLE Start
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    // 获取ble默认配置
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

    esp_bt_controller_init(&bt_cfg);
    esp_bt_controller_enable(ESP_BT_MODE_BLE);
    esp_bluedroid_config_t ble_cfg = {
        .ssp_en = false,
    };
    esp_bluedroid_init_with_cfg(&ble_cfg);
    esp_bluedroid_enable();

    // GATT的回调注册
    esp_ble_gatts_register_callback(gatts_event_handler);
    // GAP事件的函数
    esp_ble_gap_register_callback(gap_event_handler);
    // 注册APP
    esp_ble_gatts_app_register(ESP_SPP_APP_ID);

    esp_err_t local_mtu_ret = esp_ble_gatt_set_local_mtu(500);
    if (local_mtu_ret)
        ESP_LOGE(TAG_MAIN, "set local  MTU failed, error code = %x", local_mtu_ret);

    ESP_LOGI(TAG_MAIN, "BLE Started.");
    const TickType_t init_ack_timeout = 3000 / portTICK_PERIOD_MS;
    QueueHandle_t init_stop_done_queue = xQueueCreate(1, sizeof(PRM_STOP_DONE_EVENT_DATA));
    bool init_stop_done_handler_registered = false;
    if (init_stop_done_queue != NULL)
    {
        esp_event_handler_register_with(pr_events_loop_handle, PRM, PRM_STOP_DONE_EVENT, prm_stop_done_event_handler_local, init_stop_done_queue);
        init_stop_done_handler_registered = true;
    }

    auto request_init_motor_stop = [&](const char *context) -> bool {
        PRM_STOP_EVENT_DATA stop_data = {};
        PRM_STOP_DONE_EVENT_DATA stop_done_data = {};
        stop_data.address = MOTOR;
        if (init_stop_done_queue != NULL)
            xQueueReset(init_stop_done_queue);
        ESP_LOGI(TAG_MAIN, "Sending PRM_STOP_EVENT: %s", context);
        esp_event_post_to(pr_events_loop_handle, PRM, PRM_STOP_EVENT, &stop_data, sizeof(PRM_STOP_EVENT_DATA), portMAX_DELAY);
        if (!wait_send_ack(context, init_ack_timeout))
        {
            ESP_LOGW(TAG_MAIN, "Timeout waiting ACK for %s", context);
            return false;
        }
        ESP_LOGI(TAG_MAIN, "PRM_STOP_EVENT ACK received, waiting PRM_STOP_DONE_EVENT: %s", context);
        if (init_stop_done_queue == NULL || xQueueReceive(init_stop_done_queue, &stop_done_data, 3000 / portTICK_PERIOD_MS) != pdTRUE)
        {
            ESP_LOGW(TAG_MAIN, "Timeout waiting PRM_STOP_DONE_EVENT for %s", context);
            return false;
        }
        ESP_LOGI(TAG_MAIN, "PRM_STOP_DONE_EVENT received: status=%s context=%s", esp_err_to_name(stop_done_data.status), context);
        if (stop_done_data.status != ESP_OK)
        {
            ESP_LOGW(TAG_MAIN, "PRM_STOP_DONE_EVENT status not OK for %s: %s", context, esp_err_to_name(stop_done_data.status));
            return false;
        }
        return true;
    };

    // 发送STOP到各设备以复位
    if (!request_init_motor_stop("PRM_STOP_EVENT(initial)"))
        ESP_LOGW(TAG_MAIN, "Initial motor stop not confirmed");

    // 发送STOP到各装甲板设备
    PRA_STOP_EVENT_DATA pra_stop_event_data;
    for (uint8_t i = 0; i < 5; i++) // TODO: 改成5
    {
        pra_stop_event_data.address = i;
        esp_event_post_to(pr_events_loop_handle, PRA, PRA_STOP_EVENT, &pra_stop_event_data, sizeof(PRA_STOP_EVENT_DATA), portMAX_DELAY);
        if ((xEventGroupWaitBits(ESPNowProtocol::send_state, ESPNowProtocol::SEND_ACK_OK_BIT, pdTRUE, pdTRUE, init_ack_timeout) & ESPNowProtocol::SEND_ACK_OK_BIT) == 0)
        {
            ESP_LOGW(TAG_MAIN, "Timeout waiting ACK for PRA_STOP_EVENT address=%u", i);
        }
    }
    // 电机停止
    if (!request_init_motor_stop("PRM_STOP_EVENT(second)"))
        ESP_LOGW(TAG_MAIN, "Second motor stop not confirmed");

    if (init_stop_done_handler_registered)
        esp_event_handler_unregister_with(pr_events_loop_handle, PRM, PRM_STOP_DONE_EVENT, prm_stop_done_event_handler_local);
    if (init_stop_done_queue != NULL)
        vQueueDelete(init_stop_done_queue);

    // 启动LED动画，表示大符初始化完成
    xTaskCreate(led_animation_task, "led_animation_task", 2048, NULL, 5, &led_animation_task_handle);

    // 解锁电机
    PRM_UNLOCK_EVENT_DATA unlock_event_data;
    unlock_event_data.address = MOTOR;
    esp_event_post_to(pr_events_loop_handle, PRM, PRM_UNLOCK_EVENT, &unlock_event_data, sizeof(PRM_UNLOCK_EVENT_DATA), portMAX_DELAY);
    if ((xEventGroupWaitBits(ESPNowProtocol::send_state, ESPNowProtocol::SEND_ACK_OK_BIT, pdTRUE, pdTRUE, init_ack_timeout) & ESPNowProtocol::SEND_ACK_OK_BIT) == 0)
    {
        ESP_LOGW(TAG_MAIN, "Timeout waiting ACK for PRM_UNLOCK_EVENT");
    }

    vTaskSuspend(NULL);
}
