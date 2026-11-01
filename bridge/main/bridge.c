/*
 * se05x-lab-bridge — ESP32-S3 firmware: I2C target that stands in for an NXP SE05x.
 *
 * The host MCU writes a T=1-over-I2C frame to address 0x48, as it would to the chip. The bridge
 * forwards that frame over TCP to the simulator and keeps the response. NXP's T=1 stack reads a
 * response in three I2C reads (NAD+PCB, LEN, payload+CRC) and, while the first byte is not the
 * start of frame 0xA5, it simply polls again with a growing delay (up to ~20 s). So while the
 * simulator has not answered yet, every poll gets "00 00"; once the answer is here, the whole frame
 * is queued in the I2C FIFO and the host drains it with its three reads.
 *
 * Status: USB console logs; the RGB LED stays OFF and only gives short, dim flashes for events.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "driver/i2c_slave.h"
#include "led_strip.h"

#if __has_include("config.local.h")
#include "config.local.h"
#else
#error "Copy config.local.h.example to config.local.h and fill in your Wi-Fi and simulator address"
#endif
#ifndef BRIDGE_I2C_SDA
#define BRIDGE_I2C_SDA 8
#endif
#ifndef BRIDGE_I2C_SCL
#define BRIDGE_I2C_SCL 9
#endif
#ifndef BRIDGE_I2C_ADDR
#define BRIDGE_I2C_ADDR 0x48
#endif
#ifndef BRIDGE_LED_GPIO
#define BRIDGE_LED_GPIO 48
#endif

#define T1_MAX_FRAME (3 + 254 + 2)   /* NAD PCB LEN + IFSC 254 + CRC16 */
#define T1_SOF_FROM_SE 0xA5

static const char *TAG = "bridge";

/* ---------------------------------------------------------------- LED: off unless signalling */
static led_strip_handle_t led;
typedef enum { LED_OK, LED_WARN, LED_ERR } led_event_t;

static void led_init(void) {
    if (BRIDGE_LED_GPIO < 0) return;
    led_strip_config_t cfg = { .strip_gpio_num = BRIDGE_LED_GPIO, .max_leds = 1 };
    led_strip_rmt_config_t rmt = { .resolution_hz = 10 * 1000 * 1000 };
    if (led_strip_new_rmt_device(&cfg, &rmt, &led) == ESP_OK) {
        led_strip_clear(led);   /* some boards light the LED at power-up: switch it off */
    }
    else {
        led = NULL;
    }
}

/* One short, dim flash (max 12/255 per channel), then off. */
static void led_flash(led_event_t ev, int times) {
    if (!led) return;
    uint8_t r = 0, g = 0, b = 0;
    if (ev == LED_OK) g = 12; else if (ev == LED_WARN) { r = 10; g = 6; } else r = 12;
    for (int i = 0; i < times; i++) {
        led_strip_set_pixel(led, 0, r, g, b);
        led_strip_refresh(led);
        vTaskDelay(pdMS_TO_TICKS(60));
        led_strip_clear(led);
        vTaskDelay(pdMS_TO_TICKS(140));
    }
}

/* ---------------------------------------------------------------- Wi-Fi */
static EventGroupHandle_t net_events;
#define NET_UP BIT0

static void wifi_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = data;
        xEventGroupClearBits(net_events, NET_UP);
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %d, rssi %d), retrying", d->reason, d->rssi);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        ESP_LOGI(TAG, "Wi-Fi up, IP " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(net_events, NET_UP);
    }
}

static void wifi_start(void) {
    net_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_handler, NULL));
    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, BRIDGE_WIFI_SSID, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, BRIDGE_WIFI_PASSWORD, sizeof(cfg.sta.password));
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;          /* several access points: pick the best */
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));        /* latency matters more than power */
    ESP_ERROR_CHECK(esp_wifi_start());
}

/* ---------------------------------------------------------------- TCP link to the simulator */
static int sim_sock = -1;

static void sim_close(void) {
    if (sim_sock >= 0) { close(sim_sock); sim_sock = -1; }
}

static bool sim_connect(void) {
    if (sim_sock >= 0) return true;
    xEventGroupWaitBits(net_events, NET_UP, false, true, portMAX_DELAY);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(BRIDGE_SIM_PORT) };
    inet_pton(AF_INET, BRIDGE_SIM_HOST, &a.sin_addr);
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return false;
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct timeval tv = { .tv_sec = 10 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
        ESP_LOGE(TAG, "cannot reach the simulator at %s:%d", BRIDGE_SIM_HOST, BRIDGE_SIM_PORT);
        close(s);
        led_flash(LED_ERR, 2);
        return false;
    }
    sim_sock = s;
    ESP_LOGI(TAG, "connected to the simulator at %s:%d", BRIDGE_SIM_HOST, BRIDGE_SIM_PORT);
    led_flash(LED_OK, 2);
    return true;
}

static bool recv_all(uint8_t *buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        int r = recv(sim_sock, buf + got, n - got, 0);
        if (r <= 0) return false;
        got += r;
    }
    return true;
}

/* Send one T=1 frame, receive one T=1 frame. Returns the response length, or 0 on failure. */
static size_t sim_exchange(const uint8_t *req, size_t req_len, uint8_t *resp) {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!sim_connect()) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        if (send(sim_sock, req, req_len, 0) == (int)req_len && recv_all(resp, 3) &&
            recv_all(resp + 3, resp[2] + 2)) {
            return 3 + resp[2] + 2;
        }
        ESP_LOGW(TAG, "simulator link lost, reconnecting");
        sim_close();
    }
    led_flash(LED_ERR, 3);
    return 0;
}

/* ---------------------------------------------------------------- I2C target */
typedef enum { EV_FRAME, EV_READ } bridge_ev_type_t;
typedef struct {
    bridge_ev_type_t type;
    uint16_t len;
    uint8_t data[T1_MAX_FRAME];
} bridge_ev_t;

static QueueHandle_t events;
static i2c_slave_dev_handle_t i2c_dev;

static bool IRAM_ATTR on_receive(i2c_slave_dev_handle_t dev, const i2c_slave_rx_done_event_data_t *e, void *arg) {
    static bridge_ev_t ev;   /* ISR context: no large stack copies */
    BaseType_t woken = pdFALSE;
    ev.type = EV_FRAME;
    ev.len = e->length > T1_MAX_FRAME ? T1_MAX_FRAME : e->length;
    memcpy(ev.data, e->buffer, ev.len);
    xQueueSendFromISR(events, &ev, &woken);
    return woken == pdTRUE;
}

static bool IRAM_ATTR on_request(i2c_slave_dev_handle_t dev, const i2c_slave_request_event_data_t *e, void *arg) {
    static bridge_ev_t ev;
    BaseType_t woken = pdFALSE;
    ev.type = EV_READ;
    ev.len = 0;
    xQueueSendFromISR(events, &ev, &woken);
    return woken == pdTRUE;
}

static void i2c_start(void) {
    i2c_slave_config_t cfg = {
        .i2c_port = -1,
        .sda_io_num = BRIDGE_I2C_SDA,
        .scl_io_num = BRIDGE_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .send_buf_depth = 512,
        .receive_buf_depth = 512,
        .slave_addr = BRIDGE_I2C_ADDR,
        .addr_bit_len = I2C_ADDR_BIT_LEN_7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_slave_device(&cfg, &i2c_dev));
    i2c_slave_event_callbacks_t cbs = { .on_receive = on_receive, .on_request = on_request };
    ESP_ERROR_CHECK(i2c_slave_register_event_callbacks(i2c_dev, &cbs, NULL));
    ESP_LOGI(TAG, "I2C target at 0x%02X on SDA=%d SCL=%d", BRIDGE_I2C_ADDR, BRIDGE_I2C_SDA, BRIDGE_I2C_SCL);
}

static void hex(const char *what, const uint8_t *d, size_t n) {
    char line[3 * 24 + 4];
    size_t k = 0;
    for (size_t i = 0; i < n && i < 24; i++) k += snprintf(line + k, sizeof(line) - k, "%02x ", d[i]);
    if (n > 24) snprintf(line + k, sizeof(line) - k, "...");
    ESP_LOGI(TAG, "%s (%u B): %s", what, (unsigned)n, line);
}

/* The I2C task answers every read immediately (the host's bus is stretched meanwhile), and the
 * simulator task does the slow TCP exchange. They share one response slot. */
static uint8_t req[T1_MAX_FRAME], resp[T1_MAX_FRAME];
static size_t req_len, resp_len;           /* resp_len != 0: a response is ready to be read */
static portMUX_TYPE slot_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t sim_task_handle;

static void sim_task(void *arg) {
    static uint8_t local_req[T1_MAX_FRAME], local_resp[T1_MAX_FRAME];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        taskENTER_CRITICAL(&slot_lock);
        size_t n = req_len;
        memcpy(local_req, req, n);
        taskEXIT_CRITICAL(&slot_lock);
        hex("host -> SE", local_req, n);
        size_t m = sim_exchange(local_req, n, local_resp);
        if (!m) continue;
        hex("SE -> host", local_resp, m);
        taskENTER_CRITICAL(&slot_lock);
        memcpy(resp, local_resp, m);
        resp_len = m;
        taskEXIT_CRITICAL(&slot_lock);
    }
}

static void i2c_task(void *arg) {
    static bridge_ev_t ev;
    static uint8_t out[T1_MAX_FRAME];
    static const uint8_t not_ready[2] = { 0x00, 0x00 };
    uint32_t written;
    for (;;) {
        if (xQueueReceive(events, &ev, portMAX_DELAY) != pdTRUE) continue;
        if (ev.type == EV_FRAME) {
            if (ev.len < 5) continue;            /* not a T=1 frame (e.g. a bus probe) */
            taskENTER_CRITICAL(&slot_lock);
            memcpy(req, ev.data, ev.len);
            req_len = ev.len;
            resp_len = 0;                        /* a new request invalidates any old response */
            taskEXIT_CRITICAL(&slot_lock);
            xTaskNotifyGive(sim_task_handle);
        }
        else {                                   /* host reads and the FIFO is empty */
            size_t n = 0;
            taskENTER_CRITICAL(&slot_lock);
            if (resp_len && resp[0] == T1_SOF_FROM_SE) {
                n = resp_len;
                memcpy(out, resp, n);
                resp_len = 0;
            }
            taskEXIT_CRITICAL(&slot_lock);
            if (n) i2c_slave_write(i2c_dev, out, n, &written, 100);
            else   i2c_slave_write(i2c_dev, not_ready, sizeof(not_ready), &written, 100);
        }
    }
}

void app_main(void) {
    led_init();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    events = xQueueCreate(8, sizeof(bridge_ev_t));
    wifi_start();
    i2c_start();
    xTaskCreate(sim_task, "sim", 6144, NULL, 5, &sim_task_handle);
    xTaskCreate(i2c_task, "i2c", 4096, NULL, 12, NULL);
    sim_connect();                               /* connect early so the first frame is fast */
}
