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


/* WIFI STUFF THAT PROBABLY WILL BE MOVED TO A SEPARATE FILE LATER */


#define SERVO_MIN_PULSEWIDTH_US 500
#define SERVO_MAX_PULSEWIDTH_US 2400
#define SERVO_MIN_DEGREE        -90
#define SERVO_MAX_DEGREE        90
#define SERVO_GPIO              14   // your GPIO pin


#define WIFI_SSID "SpectrumSetup-1F"
#define WIFI_PASS "brightbird698"

// Event handler — called automatically when WiFi status changes
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect(); // start connecting when WiFi driver starts
    } 
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect(); // auto-reconnect if dropped
    } 
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        // We have an IP address — WiFi is ready
        printf("WiFi connected!\n");
    }
}

void wifi_init() {
    // 1. Initialize NVS (WiFi needs this to store config)
    nvs_flash_init();

    // 2. Initialize the network interface
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    // 3. Initialize WiFi driver with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    // 4. Register event handler for WiFi and IP events
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    // 5. Set credentials and start
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
// Helper to convert angle to pulse width
static inline uint32_t angle_to_us(int angle) {
    return (angle - SERVO_MIN_DEGREE) * (SERVO_MAX_PULSEWIDTH_US - SERVO_MIN_PULSEWIDTH_US)
           / (SERVO_MAX_DEGREE - SERVO_MIN_DEGREE) + SERVO_MIN_PULSEWIDTH_US;
}

void sync_time() {
    // New config-based API
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&config);

    // Set your timezone (US Central for Minnesota)
    setenv("TZ", "CST6CDT,M3.2.0,M11.1.0", 1);
    tzset();

    // Wait up to 10 seconds for sync
    printf("Waiting for time sync...\n");
    esp_netif_sntp_sync_wait(pdMS_TO_TICKS(100000));

    // Print the synced time
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    printf("Time synced: %s", asctime(&timeinfo));
}

void mcpwm_init(void){ 
    // 1. Create timer
    mcpwm_timer_handle_t timer;
    mcpwm_timer_config_t timer_cfg = {
        .group_id = 0,
        .clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,  // 1MHz → 1µs resolution
        .period_ticks = 20000,     // 20ms = 50Hz
        .count_mode = MCPWM_TIMER_COUNT_MODE_UP,
    };
    mcpwm_new_timer(&timer_cfg, &timer);

    // 2. Create operator & connect to timer
    mcpwm_oper_handle_t oper;
    mcpwm_operator_config_t oper_cfg = { .group_id = 0 };
    mcpwm_new_operator(&oper_cfg, &oper);
    mcpwm_operator_connect_timer(oper, timer);

    // 3. Create comparator
    mcpwm_cmpr_handle_t comparator;
    mcpwm_comparator_config_t cmp_cfg = { .flags.update_cmp_on_tez = true };
    mcpwm_new_comparator(oper, &cmp_cfg, &comparator);

    // 4. Create generator (GPIO output)
    mcpwm_gen_handle_t generator;
    mcpwm_generator_config_t gen_cfg = { .gen_gpio_num = SERVO_GPIO };
    mcpwm_new_generator(oper, &gen_cfg, &generator);

    // 5. Set PWM action: high on timer=0, low on compare match
    mcpwm_generator_set_action_on_timer_event(generator,
        MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH));
    mcpwm_generator_set_action_on_compare_event(generator,
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, comparator, MCPWM_GEN_ACTION_LOW));

    // 6. Start timer
    mcpwm_timer_enable(timer);

}

void app_main(void) {

    wifi_init();
    vTaskDelay(pdMS_TO_TICKS(3000));
    sync_time();
    // while(1){ 
    //     // 6. Start timer
    //     mcpwm_timer_start_stop(timer, MCPWM_TIMER_START_NO_STOP);
    //     // 7. Move servo
    //     mcpwm_comparator_set_compare_value(comparator, angle_to_us(30)); 
    //     ESP_LOGI("SERVO", "MOVING LEFT SLOWLY AT 30 DEGREES");
    //     vTaskDelay(pdMS_TO_TICKS(10000));
    //     mcpwm_comparator_set_compare_value(comparator, angle_to_us(-30)); 
    //     ESP_LOGI("SERVO", "MOVING RIGHT SLOWLY AT 30 DEGREES");
    //     vTaskDelay(pdMS_TO_TICKS(10000));
    //     // DISABLE TIMER
    //     mcpwm_timer_start_stop(timer, MCPWM_TIMER_START_STOP_EMPTY);
    //     ESP_LOGI("SERVO", "STOP");
    //     vTaskDelay(pdMS_TO_TICKS(10000));
    // }
}


