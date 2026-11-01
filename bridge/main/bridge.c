/*
 * se05x-lab-bridge — ESP32-S3 firmware: I2C target that stands in for an NXP SE05x.
 *
 * The host MCU writes a T=1-over-I2C frame to address 0x48, as it would to the chip. The bridge
 * forwards that frame over TCP to the simulator and keeps the response. NXP's T=1 stack reads a
 * response in three I2C reads (NAD+PCB, LEN, payload+CRC) and, while the first byte is not the
 * start of frame 0xA5, it simply polls again with a growing delay (up to ~20 s). So while the
 * simulator has not answered yet, polls read whatever the idle I2C target returns (never 0xA5, so
 * NXP retries); as soon as the answer arrives it is queued in the I2C TX FIFO, and the next poll
 * drains it with the usual three reads. The ESP32-S3 stretches SCL on every read address match
 * until software releases it, so each read request is released at once with a zero-length write,
 * and the response is pushed to the FIFO by the simulator task as soon as it arrives.
 *
 * Status: USB console logs; the RGB LED stays OFF and only gives short, dim flashes for events.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
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
#include "driver/gpio.h"
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
#ifndef BRIDGE_FILL_AFTER_HEADER
#define BRIDGE_FILL_AFTER_HEADER 1   /* the S3 drops the byte after a 2-byte read: see sim_task */
#endif
#define FIFO_FIRST_CHUNK 16          /* < SOC_I2C_FIFO_LEN (32): goes straight into the hardware FIFO */

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
    /* The simulator answers in milliseconds. If it does not answer (e.g. it was restarted and has
     * nothing to resend for the host's R-block), give up soon: while waiting, the bridge cannot
     * serve the host, which keeps retrying and recovers on its own. */
    struct timeval tv = { .tv_sec = 2 };
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
typedef enum { EV_FRAME, EV_READ, EV_RESET } bridge_ev_type_t;
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

static void i2c_start(void);
static void i2c_restart(void);

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
    static bool logged;
    if (!logged) {
        ESP_LOGI(TAG, "I2C target at 0x%02X on SDA=%d SCL=%d", BRIDGE_I2C_ADDR, BRIDGE_I2C_SDA, BRIDGE_I2C_SCL);
        logged = true;
    }
}

/* Fresh peripheral for every new request: empty hardware FIFO and ring buffer, so no byte left over
 * from a previous exchange can sit in front of the next response. It happens during the host's
 * first poll, which then reads junk (never 0xA5) and simply polls again. */
static void i2c_restart(void) {
    if (i2c_dev) {
        i2c_del_slave_device(i2c_dev);
        i2c_dev = NULL;
    }
    i2c_start();
}

static void hex(const char *what, const uint8_t *d, size_t n) {
    char line[3 * 24 + 4];
    size_t k = 0;
    for (size_t i = 0; i < n && i < 24; i++) k += snprintf(line + k, sizeof(line) - k, "%02x ", d[i]);
    if (n > 24) snprintf(line + k, sizeof(line) - k, "...");
    ESP_LOGI(TAG, "%s (%u B): %s", what, (unsigned)n, line);
}

/* The I2C task hands each request to the simulator task, which does the slow TCP exchange and
 * pushes the response into the I2C TX FIFO. */
static uint8_t req[T1_MAX_FRAME];
static size_t req_len;
static uint8_t out_buf[T1_MAX_FRAME + 1];   /* response ready for the host, with filler */
static size_t out_len;                     /* != 0: written to the FIFO at the next read request */
static portMUX_TYPE slot_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t sim_task_handle;
static volatile uint32_t n_reads, n_resp, n_wfail;
static volatile uint32_t n_frames;          /* frames received from the host */
static volatile bool sim_busy;              /* an exchange with the simulator is in progress */

static void sim_task(void *arg) {
    static uint8_t local_req[T1_MAX_FRAME], local_resp[T1_MAX_FRAME];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        taskENTER_CRITICAL(&slot_lock);
        size_t n = req_len;
        memcpy(local_req, req, n);
        taskEXIT_CRITICAL(&slot_lock);
        hex("host -> SE", local_req, n);
        sim_busy = true;
        size_t m = sim_exchange(local_req, n, local_resp);
        sim_busy = false;
        if (!m) continue;
        hex("SE -> host", local_resp, m);
        /* Optional filler after NAD+PCB (BRIDGE_FILL_AFTER_HEADER): a workaround for a dropped
         * byte seen before the FIFO-first write below; kept switchable to verify it is unneeded. */
        taskENTER_CRITICAL(&slot_lock);
        size_t p = 0;
        out_buf[p++] = local_resp[0];
        out_buf[p++] = local_resp[1];
        if (BRIDGE_FILL_AFTER_HEADER) out_buf[p++] = 0xFF;
        out_buf[p++] = local_resp[2];
        memcpy(out_buf + p, local_resp + 3, m - 3);
        out_len = p + m - 3;
        taskEXIT_CRITICAL(&slot_lock);
    }
}

/* Watchdog: if SCL stays low (the target stretching a read nobody released, e.g. the host was
 * reset or re-flashed mid-transfer), the bus is stuck for everyone. Re-create the peripheral. */
/* Second check: the host keeps polling (reads) but none of its writes arrives and no answer is
 * pending. Seen once: the target kept serving reads but no longer delivered received frames, so the
 * host waited for an answer to frames the bridge never saw, and the key stalled until the bridge was
 * reset. A host only polls this long after writing a frame, so re-create the peripheral. */
static void watchdog_task(void *arg) {
    int low = 0, deaf = 0;
    uint32_t prev_reads = 0, prev_frames = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250));
        low = gpio_get_level(BRIDGE_I2C_SCL) == 0 ? low + 1 : 0;
        if (low == 6) {                          /* 1.5 s */
            static bridge_ev_t ev = { .type = EV_RESET };
            ESP_LOGW(TAG, "SCL held low for 1.5 s: resetting the I2C target");
            xQueueSend(events, &ev, 0);
            led_flash(LED_WARN, 1);
        }
        uint32_t r = n_reads, f = n_frames;
        bool polling = r - prev_reads > 20;      /* > 80 reads/s */
        deaf = (polling && f == prev_frames && !sim_busy && out_len == 0) ? deaf + 1 : 0;
        prev_reads = r;
        prev_frames = f;
        if (deaf == 12) {                        /* 3 s */
            static bridge_ev_t ev = { .type = EV_RESET };
            ESP_LOGW(TAG, "host polling for 3 s with no frame received: resetting the I2C target");
            xQueueSend(events, &ev, 0);
            led_flash(LED_WARN, 2);
            deaf = 0;
        }
    }
}

static void stats_task(void *arg) {
    uint32_t prev = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        if (n_reads + n_resp != prev) {
            ESP_LOGI(TAG, "read requests seen %lu, responses queued %lu, queue errors %lu",
                     (unsigned long)n_reads, (unsigned long)n_resp, (unsigned long)n_wfail);
            prev = n_reads + n_resp;
        }
    }
}

/* CRC-16/X-25 of T=1 over I2C (UM11225), sent least significant byte first. */
static uint16_t t1_crc(const uint8_t *d, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= d[i];
        for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
    }
    return crc ^ 0xFFFF;
}

/* A frame as the host meant it: NAD PCB LEN, LEN bytes, CRC that checks. A write cut short (seen when
 * the I2C receive FIFO is not drained in time while Wi-Fi is busy) is dropped: forwarded, it would
 * leave the simulator waiting for the missing bytes. Unanswered, the host retransmits by itself. */
static uint32_t n_bad_frames;
static bool t1_frame_ok(const uint8_t *d, size_t n) {
    if (n < 5 || n != 3u + d[2] + 2u) return false;
    uint16_t crc = t1_crc(d, n - 2);
    return d[n - 2] == (crc & 0xFF) && d[n - 1] == (crc >> 8);
}

static void i2c_task(void *arg) {
    static bridge_ev_t ev;
    for (;;) {
        if (xQueueReceive(events, &ev, portMAX_DELAY) != pdTRUE) continue;
        if (ev.type == EV_RESET) {
            taskENTER_CRITICAL(&slot_lock);
            out_len = 0;
            taskEXIT_CRITICAL(&slot_lock);
            i2c_restart();
            continue;
        }
        if (ev.type == EV_FRAME) {
            if (ev.len < 5) continue;            /* not a T=1 frame (e.g. a bus probe) */
            n_frames++;
            if (!t1_frame_ok(ev.data, ev.len)) {
                n_bad_frames++;
                ESP_LOGW(TAG, "dropping a damaged frame (%u B, LEN %u), the host will retransmit (%lu so far)",
                         (unsigned)ev.len, ev.data[2], (unsigned long)n_bad_frames);
                i2c_restart();
                xQueueReset(events);
                continue;
            }
            taskENTER_CRITICAL(&slot_lock);
            memcpy(req, ev.data, ev.len);
            req_len = ev.len;
            out_len = 0;                          /* a new request discards any stale response */
            taskEXIT_CRITICAL(&slot_lock);
            i2c_restart();
            xQueueReset(events);                  /* drop the read request of the old peripheral */
            xTaskNotifyGive(sim_task_handle);
        }
        else {
            /* The S3 stretches SCL on every read address match until software releases it, even if
             * the FIFO already holds data. A zero-length write releases the stretch (and tops the
             * FIFO up from the driver's ring buffer). If no response is queued yet, the host reads
             * whatever the idle target returns, never 0xA5, and NXP's stack polls again. */
            static uint8_t local[T1_MAX_FRAME + 1];
            size_t n = 0;
            uint32_t w;
            n_reads++;
            taskENTER_CRITICAL(&slot_lock);
            if (out_len) { n = out_len; memcpy(local, out_buf, n); out_len = 0; }
            taskEXIT_CRITICAL(&slot_lock);
            /* Writing here, while SCL is stretched for this read, is the only moment the target is
             * idle and waiting; a zero-length write just releases the stretch. */
            /* i2c_slave_write() only puts data straight into the 32-byte hardware FIFO when ALL of
             * it fits; otherwise everything goes to a ring buffer that an interrupt drains into the
             * FIFO later, at a random point relative to the host's reads (empty reads, shifted
             * bytes). So the first chunk is small enough to land in the FIFO before the stretch is
             * released, and the rest follows. */
            esp_err_t e;
            size_t first = n > FIFO_FIRST_CHUNK ? FIFO_FIRST_CHUNK : n;
            uint32_t w2 = 0;
            e = i2c_slave_write(i2c_dev, local, first, &w, 100);
            if (e == ESP_OK && n > first) {
                e = i2c_slave_write(i2c_dev, local + first, n - first, &w2, 100);
                w += w2;
            }
            if (e != ESP_OK || w != n) n_wfail++;
            if (n) {
                n_resp++;
                ESP_EARLY_LOGI(TAG, "pushed response at read request #%lu: %u B, accepted %lu (%s)",
                               (unsigned long)n_reads, (unsigned)n, (unsigned long)w, esp_err_to_name(e));
            }
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
    xTaskCreate(stats_task, "stats", 3072, NULL, 2, NULL);
    xTaskCreate(watchdog_task, "wdog", 3072, NULL, 3, NULL);
    sim_connect();                               /* connect early so the first frame is fast */
}
