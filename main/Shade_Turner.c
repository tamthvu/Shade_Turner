#include "driver/mcpwm_prelude.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h" 
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "time.h"
#include "esp_netif_sntp.h"

#define SERVO_MIN_PULSEWIDTH_US 500
#define SERVO_MAX_PULSEWIDTH_US 2400
#define SERVO_MIN_DEGREE        -90
#define SERVO_MAX_DEGREE        90
#define SERVO_GPIO              14

#define WIFI_SSID "SpectrumSetup-1F"
#define WIFI_PASS "brightbird698"

// --- Globals ---
mcpwm_timer_handle_t timer;
mcpwm_cmpr_handle_t comparator;
int current_angle = 0;

// --- Schedule ---
typedef struct {
    int hour;
    int minute;
    int angle;
    int speed_ms;
    char *label;
} servo_schedule_t;

servo_schedule_t schedule[] = {
    {  7, 00,  90, 15, "Morning open  - clockwise"    },  // 7:00 AM → clockwise to 90°
    { 19, 00, -90, 15, "Evening close - anticlockwise" }, // 7:00 PM → anticlockwise to -90°
};
int schedule_count = sizeof(schedule) / sizeof(schedule[0]);

// --- WiFi ---
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        printf("WiFi connected!\n");
    }
}

void wifi_init() {
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
}

// --- Time Sync ---
void sync_time() {
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&config);

    setenv("TZ", "CST6CDT,M3.2.0,M11.1.0", 1);
    tzset();

    printf("Waiting for time sync...\n");
    esp_err_t ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(100000));

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    if (ret == ESP_OK) {
        printf("Time synced: %s", asctime(&timeinfo));
    } else {
        printf("Time sync FAILED\n");
    }
}

// --- MCPWM Init ---
void mcpwm_init(void) {
    mcpwm_timer_config_t timer_cfg = {
        .group_id = 0,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,
        .period_ticks = 20000,
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
    };
    mcpwm_new_timer(&timer_cfg, &timer);

    mcpwm_oper_handle_t oper;
    mcpwm_operator_config_t oper_cfg = { .group_id = 0 };
    mcpwm_new_operator(&oper_cfg, &oper);
    mcpwm_operator_connect_timer(oper, timer);

    mcpwm_comparator_config_t cmp_cfg = { .flags.update_cmp_on_tez = true };
    mcpwm_new_comparator(oper, &cmp_cfg, &comparator);

    mcpwm_gen_handle_t generator;
    mcpwm_generator_config_t gen_cfg = { .gen_gpio_num = SERVO_GPIO };
    mcpwm_new_generator(oper, &gen_cfg, &generator);

    mcpwm_generator_set_action_on_timer_event(generator,
        MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH));
    mcpwm_generator_set_action_on_compare_event(generator,
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, comparator, MCPWM_GEN_ACTION_LOW));

    mcpwm_timer_enable(timer);
}

// --- Angle Helper ---
static inline uint32_t angle_to_us(int angle) {
    return (angle - SERVO_MIN_DEGREE) * (SERVO_MAX_PULSEWIDTH_US - SERVO_MIN_PULSEWIDTH_US)
           / (SERVO_MAX_DEGREE - SERVO_MIN_DEGREE) + SERVO_MIN_PULSEWIDTH_US;
}

// --- Servo Movement ---
void move_servo(int target_angle, int speed_ms, char *label) {
    printf("Starting: %s\n", label);

    // Start timer so pulses begin
    mcpwm_timer_start_stop(timer, MCPWM_TIMER_START_NO_STOP);

    // Move to target
    mcpwm_comparator_set_compare_value(comparator, angle_to_us(current_angle));

    vTaskDelay(pdMS_TO_TICKS(5000));

    // Stop the timer — servo goes limp but stays at angle
    mcpwm_timer_start_stop(timer, MCPWM_TIMER_START_STOP_EMPTY);
    printf("Timer stopped stop rotating);
}

// --- Main ---
void app_main(void) {
    wifi_init();
    vTaskDelay(pdMS_TO_TICKS(3000));
    sync_time();
    mcpwm_init();

    int last_triggered_minute = -1;

    while (1) {
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);

        int hour   = timeinfo.tm_hour;
        int minute = timeinfo.tm_min;

        for (int i = 0; i < schedule_count; i++) {
            if (hour   == schedule[i].hour    &&
                minute == schedule[i].minute   &&
                minute != last_triggered_minute) {

                move_servo(schedule[i].angle, schedule[i].speed_ms, schedule[i].label);
                last_triggered_minute = minute;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}