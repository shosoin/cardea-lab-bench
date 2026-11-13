/*
 * Lab bench — emulator of a GROW R502-F Pro fingerprint module, so the host firmware's fingerprint
 * path can be developed and tested without the sensor (and without a person touching it).
 *
 * Written from the module's user manual, "R502F-Pro 指纹识别模块用户手册" V1.1 (2024-03, hzgrow.com).
 * Where the manual is silent or contradicts itself the choice made here is marked "UNVERIFIED" and
 * listed in the README; the real sensor is the reference and wins.
 *
 * What is emulated:
 *  - UART, 57600 8N1 by default (9600 x N, N = 1/2/4/6/12, applied at the next power-on).
 *  - Packets: EF01 | address (4) | packet id (1) | length (2) | payload | checksum (2), big endian;
 *    checksum = 16-bit sum of packet id, length and payload. Commands 0x01, answers 0x07, data
 *    0x02 (more follow) / 0x08 (last). Packets for another address are ignored.
 *  - All 31 documented commands: GetImage, GenChar, Match, Search, RegModel, StoreChar, LoadChar,
 *    UpChar, DownChar, UpImage, DownImage, DeleteChar, Empty, SetSysPara, ReadSysPara, SetPwd,
 *    VfyPwd, GetRandomCode, SetAddr, ReadInfPage, WriteNotepad, ReadNotepad, ValidTemplateNum,
 *    ReadIndexTable, Cancel, HandShake, GetAlgVer, GetFwVer, AuraLedConfig, AutoEnroll,
 *    AutoIdentify. Anything else answers 0xFC (unsupported).
 *  - RAM (lost at power-off): one image buffer (192x192, 4 bits per pixel as on the wire) and six
 *    character buffers. "Flash" (kept in NVS, survives power-off and a reboot of this chip):
 *    template library, password, address, system parameters, 512-byte notepad.
 *  - Password: as on the real R502-F Pro (measured 2026-11-13), a password set gates nothing:
 *    every command answers after power-on; VfyPwd only says 0x00 / 0x13, with no lockout.
 *    "fp opt pwdgate 1" makes VfyPwd mandatory first (0x21), as the manual suggests. There is no
 *    encryption: the R502-F Pro has none.
 *  - Power: the host switches the module's main supply (LAB_FP_POWER_GPIO). Off: no answers, TX
 *    floats, RAM lost. On: 50 ms later the module sends 0x55 and takes commands. Switching it
 *    back on less than 2 s after switching it off is counted and logged (the manual forbids it).
 *  - WAKEUP (touch output, powered separately): low while a finger is on the sensor, also with
 *    the main supply off.
 *
 * Fingers and templates are synthetic. A "finger" is a number (A..Z = 1..26); its images and
 * templates are generated from it, so the same finger always matches itself and nothing else.
 * Templates are 512 bytes ("FPE1", kind, count, finger, filler, sum); images carry a small header
 * ("FPI1", finger, quality) in their first bytes. Data downloaded by the host in another format is
 * dropped in silence, the buffer keeping what it held (DownChar still answers 0x00, as the real
 * module does); a downloaded image in another format gives 0x06 at GenChar. Match: a template
 * against itself 409, two merged templates of a finger no match, a capture against its finger's
 * template the opt score (all as measured on the real module, 2026-11-13).
 *
 * Console ("fp ...", replies start with "BENCH"):
 *   fp status                        power, finger, settings, library size, busy operation
 *   fp finger <F> [good|messy|small|fail]  put finger F (A..Z or 1..65534) on the sensor;
 *                                    messy/small: image taken but no features (0x06/0x07);
 *                                    fail: the image cannot be taken (0x03)
 *   fp finger off | fp lift          lift the finger
 *   fp tap <F> [n] [on_ms] [off_ms] [quality]  n touches in the background (default 1, 600, 600)
 *   fp power [on|off|pin]            force the main supply, or follow the host's line (default)
 *   fp unplug | fp plug              module disconnected: no answers, TX and WAKEUP float
 *   fp next <cmd|any> <fault> [arg]  fault on the next answer to cmd (hex code or name):
 *                                    code XX | silent | badsum | short | garbage | delay MS | reboot |
 *                                    page N (answer "ok, slot N": a Search that ignored its range)
 *   fp next clear | fp next          drop / count the queued faults
 *   fp led [clear]                   last LED ring commands (host's and the module's own)
 *   fp db | fp db set <slot> <F> | fp db del <slot>   library: list / add / delete
 *   fp factory                       erase the emulated flash (library, password, address...)
 *   fp opt speed fast|real | score N | timeout MS | trace 0|1|2 | pwdgate 0|1
 *   fp stats [clear]                 command / error counters
 *
 * Test bench only: it shows on purpose that an external fingerprint module can be impersonated by
 * anything that speaks its UART.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "soc/uart_periph.h"
#include "soc/gpio_sig_map.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "lab_console.h"
#include "fp_emu.h"

#define UARTN       CONFIG_LAB_FP_UART_NUM
#define TX_GPIO     CONFIG_LAB_FP_TX_GPIO
#define RX_GPIO     CONFIG_LAB_FP_RX_GPIO
#define WAKE_GPIO   CONFIG_LAB_FP_WAKEUP_GPIO
#define PWR_GPIO    CONFIG_LAB_FP_POWER_GPIO
#ifdef CONFIG_LAB_FP_POWER_ACTIVE_LOW
#define PWR_ON_LEVEL 0
#else
#define PWR_ON_LEVEL 1
#endif
#define CAP         CONFIG_LAB_FP_CAPACITY

#define IMG_W       192
#define IMG_H       192
#define IMG_BYTES   (IMG_W * IMG_H / 2)
#define TPL_BYTES   512
#define NBUF        6
#define INFO_BYTES  512
#define BOOT_MS     50
#define MIN_OFF_MS  2000
#define FRAME_GAP_MS 200      /* a frame stalled this long mid-way is dropped */
#define MAX_BODY    (256 + 2)

/* Confirmation codes (manual, table 4.3) */
enum {
    C_OK = 0x00, C_PKT = 0x01, C_NOFINGER = 0x02, C_IMGFAIL = 0x03, C_MESSY = 0x06,
    C_FEWPTS = 0x07, C_NOMATCH = 0x08, C_NOTFOUND = 0x09, C_MERGE = 0x0A, C_RANGE = 0x0B,
    C_LOADBAD = 0x0C, C_UPCHAR = 0x0D, C_NORX = 0x0E, C_NOTX = 0x0F, C_DELFAIL = 0x10,
    C_BADPWD = 0x13, C_NOIMG = 0x15, C_BADREG = 0x1A, C_BADDATA = 0x1B, C_FULL = 0x1F,
    C_NEEDPWD = 0x21, C_TPLBUSY = 0x22, C_DBEMPTY = 0x24, C_TIMEOUT = 0x26, C_EXISTS = 0x27,
    C_UNSUPP = 0xFC,
};

enum { PID_CMD = 0x01, PID_DATA = 0x02, PID_ACK = 0x07, PID_END = 0x08 };

enum { Q_GOOD, Q_MESSY, Q_SMALL, Q_FAIL };
static const char *const q_names[] = { "good", "messy", "small", "fail" };

static const char *TAG = "fp_emu";

/* ---------------------------------------------------------------- emulated flash (NVS) */

#define FL_MAGIC 0x46504531u   /* "FPE1" */
typedef struct {
    uint32_t magic;
    uint32_t addr;
    uint32_t pwd;
    uint8_t baud_n, security, pkt_code, _pad;
    uint8_t notepad[16][32];
    uint16_t finger[CAP];      /* 0 = empty slot */
    uint8_t meta[CAP];         /* kind << 4 | merged count (low nibble, saturates at 15) */
    uint8_t variant[CAP];      /* which template of that finger (two enrolments never match) */
} flash_t;

static flash_t fl;
static nvs_handle_t nvs;

static void flash_defaults(void) {
    memset(&fl, 0, sizeof(fl));
    fl.magic = FL_MAGIC;
    fl.addr = 0xFFFFFFFF;
    fl.pwd = 0;
    fl.baud_n = 6;             /* 57600 */
    fl.security = 3;           /* UNVERIFIED: factory security level not given in the manual */
    fl.pkt_code = 2;           /* 128 bytes */
}

static void flash_save(void) {
    esp_err_t e = nvs_set_blob(nvs, "flash", &fl, sizeof(fl));
    if (e == ESP_OK) e = nvs_commit(nvs);
    if (e != ESP_OK) ESP_LOGE(TAG, "saving the emulated flash: %s", esp_err_to_name(e));
}

static void flash_load(void) {
    size_t sz = sizeof(fl);
    if (nvs_open("fp_emu", NVS_READWRITE, &nvs) != ESP_OK) {
        ESP_LOGE(TAG, "NVS unavailable: the emulated flash will not survive a reboot");
        flash_defaults();
        return;
    }
    if (nvs_get_blob(nvs, "flash", &fl, &sz) != ESP_OK || sz != sizeof(fl) || fl.magic != FL_MAGIC) {
        flash_defaults();
        flash_save();
    }
}

static int lib_count(void) {
    int n = 0;
    for (int i = 0; i < CAP; i++) n += fl.finger[i] != 0;
    return n;
}

/* ---------------------------------------------------------------- shared state */

static SemaphoreHandle_t mx;             /* emulator state; the console takes it too */

/* Sensor surface (written by the console, read by the module) */
static volatile uint16_t finger_on;      /* 0 = no finger */
static volatile uint8_t finger_q = Q_GOOD;

/* Module */
static volatile bool present = true;     /* false: "unplugged" */
static volatile int power_force = -1;    /* -1: follow the host's line; 0/1 forced */
static bool powered;
static int64_t off_since_ms = -MIN_OFF_MS;
static uint32_t baud_active = 57600;
static int pkt_active = 128;
static bool pwd_ok;
static bool last_pass;                   /* status register bit 1 */
static uint8_t cur_cmd;                  /* command being answered (for fault matching) */

/* RAM buffers */
static uint8_t img[IMG_BYTES];
static bool img_valid;
typedef enum { CB_EMPTY, CB_VALID, CB_FOREIGN } cb_state_t;
typedef struct {
    cb_state_t st;
    uint8_t kind;          /* 1 = feature (one image), 2 = template (merged) */
    uint8_t count;
    uint16_t finger;
    uint8_t variant;       /* every capture and every merged template gets its own (1..255) */
    uint8_t raw[TPL_BYTES];
} charbuf_t;
static charbuf_t cb[NBUF + 1];           /* 1..6 */

/* Download from the host (DownChar / DownImage); a template lands in dl_tpl first */
static struct { int what; int buf; size_t got; bool overflow; } dl;   /* what: 0, 'c', 'i' */
static uint8_t dl_tpl[TPL_BYTES];

/* Options */
static bool fast;                        /* no processing delays */
static uint16_t opt_score = 150;         /* MatchScore of a match (the manual's example: 0xBD) */
#define SCORE_SAME 409                   /* real R502-F Pro: a template against itself, always */
static bool opt_pwdgate = false;         /* real module: a password set gates nothing (2026-11-13) */
static uint32_t opt_timeout_ms = 10000;  /* AutoEnroll/AutoIdentify; UNVERIFIED: text 10 s, chart 15 s */
static int trace = 1;

/* Statistics */
static struct {
    uint32_t cmds, bad_sum, other_addr, bad_len, bad_pwd, unsupported, busy_drop, power_ons,
             fast_cycles, faults;
} st;

/* LED ring log */
typedef struct { uint32_t t_ms; char src; uint8_t ctrl, speed, color, times; } led_ev_t;
#define LED_LOG 32
static led_ev_t led_log[LED_LOG];
static uint32_t led_n;

/* Faults */
typedef enum { F_CODE, F_SILENT, F_BADSUM, F_SHORT, F_GARBAGE, F_DELAY, F_REBOOT, F_PAGE } fault_kind_t;
static const char *const fault_names[] = { "code", "silent", "badsum", "short", "garbage", "delay", "reboot", "page" };
#define N_FAULT_KINDS 8
typedef struct { int cmd; fault_kind_t k; int arg; } fault_t;
#define MAX_FAULTS 8
static fault_t faults[MAX_FAULTS];
static int n_faults;

/* Automatic operations */
typedef enum { AO_NONE, AO_ENROLL, AO_IDENT } ao_kind_t;
typedef enum { PH_WAIT_FINGER, PH_GEN, PH_WAIT_LIFT, PH_SEARCH } ao_phase_t;
static struct {
    ao_kind_t kind;
    ao_phase_t phase;
    int64_t t0, due;
    /* AutoEnroll */
    uint16_t id;
    bool overwrite, dup_ok, steps, lift;
    int capture;                         /* 1..6 */
    uint16_t fingers[6];
    /* AutoIdentify */
    uint8_t grade, max_err, errs;
    uint16_t start, num;
} ao;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

static void work_delay(int ms) {
    if (!fast && ms > 0) vTaskDelay(pdMS_TO_TICKS(ms));
}

/* ---------------------------------------------------------------- names */

static const struct { uint8_t code; const char *name; } cmd_names[] = {
    {0x01, "GetImage"}, {0x02, "GenChar"}, {0x03, "Match"}, {0x04, "Search"}, {0x05, "RegModel"},
    {0x06, "StoreChar"}, {0x07, "LoadChar"}, {0x08, "UpChar"}, {0x09, "DownChar"},
    {0x0A, "UpImage"}, {0x0B, "DownImage"}, {0x0C, "DeleteChar"}, {0x0D, "Empty"},
    {0x0E, "SetSysPara"}, {0x0F, "ReadSysPara"}, {0x12, "SetPwd"}, {0x13, "VfyPwd"},
    {0x14, "GetRandomCode"}, {0x15, "SetAddr"}, {0x16, "ReadInfPage"}, {0x18, "WriteNotepad"},
    {0x19, "ReadNotepad"}, {0x1D, "ValidTemplateNum"}, {0x1F, "ReadIndexTable"}, {0x30, "Cancel"},
    {0x31, "AutoEnroll"}, {0x32, "AutoIdentify"}, {0x35, "AuraLedConfig"}, {0x39, "GetAlgVer"},
    {0x3A, "GetFwVer"}, {0x40, "HandShake"},
};

static const char *cmd_name(uint8_t c) {
    for (size_t i = 0; i < sizeof(cmd_names) / sizeof(cmd_names[0]); i++)
        if (cmd_names[i].code == c) return cmd_names[i].name;
    return "?";
}

static int cmd_by_name(const char *s) {
    if (!strcasecmp(s, "any")) return -1;
    for (size_t i = 0; i < sizeof(cmd_names) / sizeof(cmd_names[0]); i++)
        if (!strcasecmp(cmd_names[i].name, s)) return cmd_names[i].code;
    char *end;
    long v = strtol(s, &end, 16);
    if (*end || v < 0 || v > 0xFF) return -2;
    return (int)v;
}

static void fname(uint16_t f, char *out, size_t n) {
    if (f >= 1 && f <= 26) snprintf(out, n, "%c", 'A' + f - 1);
    else snprintf(out, n, "%u", f);
}

static int parse_finger(const char *s) {
    if (!s || !*s) return -1;
    if (isalpha((unsigned char)s[0]) && !s[1]) return toupper((unsigned char)s[0]) - 'A' + 1;
    char *end;
    long v = strtol(s, &end, 10);
    if (*end || v < 1 || v > 0xFFFE) return -1;
    return (int)v;
}

static int parse_quality(const char *s) {
    if (!s) return Q_GOOD;
    for (int i = 0; i < 4; i++) if (!strcasecmp(s, q_names[i])) return i;
    return -1;
}

/* ---------------------------------------------------------------- synthetic data */

static uint16_t sum16(const uint8_t *d, size_t n) {
    uint32_t s = 0;
    for (size_t i = 0; i < n; i++) s += d[i];
    return (uint16_t)s;
}

static void tpl_make(uint8_t *t, uint16_t finger, uint8_t kind, uint8_t count, uint8_t variant) {
    t[0] = 'F'; t[1] = 'P'; t[2] = 'E'; t[3] = '1';
    t[4] = kind; t[5] = count;
    t[6] = finger >> 8; t[7] = finger & 0xFF;
    t[8] = variant;
    uint32_t x = 0x9E3779B9u ^ ((uint32_t)finger << 8) ^ kind ^ ((uint32_t)variant << 24);
    for (int i = 9; i < TPL_BYTES - 2; i++) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        t[i] = (uint8_t)x;
    }
    uint16_t s = sum16(t, TPL_BYTES - 2);
    t[TPL_BYTES - 2] = s >> 8; t[TPL_BYTES - 1] = s & 0xFF;
}

/* A downloaded template is taken if its header makes sense; the rest is not checked (the real
 * module took a template with one byte changed and re-encoded it on StoreChar, 2026-11-13). */
static bool tpl_parse(const uint8_t *t, uint16_t *finger, uint8_t *kind, uint8_t *count, uint8_t *variant) {
    if (memcmp(t, "FPE1", 4)) return false;
    *kind = t[4]; *count = t[5];
    *finger = (uint16_t)(t[6] << 8 | t[7]);
    *variant = t[8];
    return *finger != 0 && (*kind == 1 || *kind == 2);
}

/* Concentric ridges around a centre that depends on the finger; header in the first 12 bytes. */
static void img_make(uint16_t finger, uint8_t q) {
    float cx = 96 + (float)((finger * 13) % 41) - 20, cy = 96 + (float)((finger * 7) % 41) - 20;
    float period = 7.0f + (float)(finger % 5);
    uint32_t x = finger * 2654435761u + 1;
    for (int y = 0; y < IMG_H; y++) {
        for (int c = 0; c < IMG_W; c += 2) {
            uint8_t px[2];
            for (int k = 0; k < 2; k++) {
                float dx = (float)(c + k) - cx, dy = (float)y - cy;
                float d = sqrtf(dx * dx + dy * dy);
                uint8_t v = (fmodf(d, period) < period / 2) ? 0x3 : 0xC;
                if (q == Q_SMALL && d > 28) v = 0xF;
                if (q == Q_MESSY) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; v = x & 0xF; }
                px[k] = v;
            }
            img[(y * IMG_W + c) / 2] = (uint8_t)(px[0] << 4 | px[1]);
        }
    }
    uint8_t *h = img;
    h[0] = 'F'; h[1] = 'P'; h[2] = 'I'; h[3] = '1';
    h[4] = finger >> 8; h[5] = finger & 0xFF; h[6] = q;
    h[7] = h[8] = h[9] = h[10] = 0;
    h[11] = (uint8_t)sum16(h, 11);
    img_valid = true;
}

static bool img_parse(uint16_t *finger, uint8_t *q) {
    if (memcmp(img, "FPI1", 4) || img[11] != (uint8_t)sum16(img, 11) || img[6] > Q_FAIL) return false;
    *finger = (uint16_t)(img[4] << 8 | img[5]);
    *q = img[6];
    return *finger != 0;
}

static void cb_set(int b, uint16_t finger, uint8_t kind, uint8_t count, uint8_t variant) {
    cb[b].st = CB_VALID;
    cb[b].finger = finger;
    cb[b].kind = kind;
    cb[b].count = count;
    cb[b].variant = variant;
    tpl_make(cb[b].raw, finger, kind, count, variant);
}

/* A new merged template: its own variant (1..255), so two enrolments of a finger differ. */
static uint8_t new_variant(void) { return (uint8_t)(1 + esp_random() % 255); }

/* ---------------------------------------------------------------- LED log */

static void led_record(char src, uint8_t ctrl, uint8_t speed, uint8_t color, uint8_t times) {
    led_log[led_n % LED_LOG] = (led_ev_t){ (uint32_t)now_ms(), src, ctrl, speed, color, times };
    led_n++;
}

/* The module's own LED signalling during AutoEnroll / AutoIdentify (charts 6.3 / 6.4). */
enum { L_BREATH = 1, L_FLASH, L_ON, L_OFF, L_FADEIN, L_FADEOUT };
enum { COL_RED = 1, COL_BLUE, COL_PURPLE, COL_GREEN, COL_YELLOW, COL_CYAN, COL_WHITE };
static void led_auto(uint8_t ctrl, uint8_t color) { led_record('m', ctrl, 0, color, 0); }

static const char *led_ctrl_name(uint8_t c) {
    static const char *const n[] = { "?", "breath", "flash", "on", "off", "fadein", "fadeout" };
    return c <= 6 ? n[c] : "?";
}

static const char *led_color_name(uint8_t c) {
    static const char *const n[] = { "?", "red", "blue", "purple", "green", "yellow", "cyan", "white" };
    if (c <= 7) return n[c];
    if (c == 0x20) return "3col";
    if (c == 0x30) return "7col";
    return "?";
}

/* ---------------------------------------------------------------- UART lines */

static void tx_attach(bool on) {
    if (on) {
        gpio_set_level(TX_GPIO, 1);                     /* idle level first: no false start bit */
        gpio_set_direction(TX_GPIO, GPIO_MODE_OUTPUT);
        esp_rom_gpio_connect_out_signal(TX_GPIO, UART_PERIPH_SIGNAL(UARTN, SOC_UART_TX_PIN_IDX), false, false);
    }
    else {
        esp_rom_gpio_connect_out_signal(TX_GPIO, SIG_GPIO_OUT_IDX, false, false);
        gpio_set_direction(TX_GPIO, GPIO_MODE_INPUT);   /* floats: an unpowered module drives nothing */
        gpio_set_pull_mode(TX_GPIO, GPIO_FLOATING);
    }
}

static void wake_update(void) {
    if (WAKE_GPIO < 0) return;
    if (!present) {
        gpio_set_direction(WAKE_GPIO, GPIO_MODE_INPUT);
        return;
    }
    gpio_set_level(WAKE_GPIO, finger_on ? 0 : 1);
    gpio_set_direction(WAKE_GPIO, GPIO_MODE_INPUT_OUTPUT);   /* input too: "fp status" reads the line */
}

/* ---------------------------------------------------------------- packets out */

static void hexlog(const char *dir, const uint8_t *d, size_t n) {
    if (trace < 2) return;
    char line[3 * 48 + 8];
    size_t k = 0;
    for (size_t i = 0; i < n && i < 48; i++) k += snprintf(line + k, sizeof(line) - k, "%02X ", d[i]);
    if (n > 48) snprintf(line + k, sizeof(line) - k, "... (%u)", (unsigned)n);
    ESP_LOGI(TAG, "%s %s", dir, line);
}

static size_t build(uint8_t *out, uint8_t pid, const uint8_t *d, size_t n) {
    uint32_t a = fl.addr;
    out[0] = 0xEF; out[1] = 0x01;
    out[2] = a >> 24; out[3] = a >> 16; out[4] = a >> 8; out[5] = a;
    out[6] = pid;
    uint16_t len = (uint16_t)(n + 2);
    out[7] = len >> 8; out[8] = len & 0xFF;
    memcpy(out + 9, d, n);
    uint16_t s = sum16(out + 6, 3 + n);
    out[9 + n] = s >> 8; out[10 + n] = s & 0xFF;
    return 11 + n;
}

static void uart_out(const uint8_t *p, size_t n) {
    hexlog("tx", p, n);
    uart_write_bytes(UARTN, p, n);
}

static void send_packet(uint8_t pid, const uint8_t *d, size_t n) {
    uint8_t out[11 + MAX_BODY];
    uart_out(out, build(out, pid, d, n));
}

static void module_ram_reset(void);

static bool fault_take(int cmd, fault_t *out) {
    for (int i = 0; i < n_faults; i++) {
        if (faults[i].cmd == -1 || faults[i].cmd == cmd) {
            *out = faults[i];
            memmove(&faults[i], &faults[i + 1], (n_faults - i - 1) * sizeof(fault_t));
            n_faults--;
            st.faults++;
            return true;
        }
    }
    return false;
}

/* Sends an answer packet (with any queued fault). True if the host got it with its code intact,
 * i.e. a data phase may follow. */
static bool send_reply(const uint8_t *d, size_t n) {
    uint8_t buf[64];
    uint8_t out[11 + 64];
    memcpy(buf, d, n);
    fault_t f;
    if (fault_take(cur_cmd, &f)) {
        ESP_LOGW(TAG, "fault on %s answer: %s %d", cmd_name(cur_cmd), fault_names[f.k], f.arg);
        switch (f.k) {
        case F_SILENT:
            return false;
        case F_CODE:
            buf[0] = (uint8_t)f.arg;
            send_packet(PID_ACK, buf, n);
            return f.arg == C_OK;
        case F_BADSUM: {
            size_t m = build(out, PID_ACK, buf, n);
            out[m - 1] ^= 0x5A;
            uart_out(out, m);
            return false;
        }
        case F_SHORT: {
            size_t m = build(out, PID_ACK, buf, n);
            uart_out(out, m / 2);
            return false;
        }
        case F_GARBAGE: {
            uint8_t g[8];
            esp_fill_random(g, sizeof(g));
            g[0] = 0x00;                      /* never a valid header by accident */
            uart_out(g, 1 + esp_random() % 7);
            break;
        }
        case F_DELAY:
            vTaskDelay(pdMS_TO_TICKS(f.arg));
            break;
        case F_PAGE:                          /* a match at that slot, whatever was asked */
            buf[0] = C_OK;
            buf[1] = (uint8_t)(f.arg >> 8);
            buf[2] = (uint8_t)f.arg;
            if (n < 5) {
                buf[3] = 0;
                buf[4] = 200;
                n = 5;
            }
            break;
        case F_REBOOT:
            module_ram_reset();
            vTaskDelay(pdMS_TO_TICKS(BOOT_MS));
            uart_write_bytes(UARTN, "\x55", 1);
            return false;
        }
    }
    send_packet(PID_ACK, buf, n);
    return true;
}

static bool reply(uint8_t code) { return send_reply(&code, 1); }

static bool reply_with(uint8_t code, const uint8_t *extra, size_t n) {
    uint8_t b[64];
    b[0] = code;
    memcpy(b + 1, extra, n);
    return send_reply(b, n + 1);
}

static void send_data(const uint8_t *d, size_t n) {
    size_t off = 0;
    while (off < n) {
        size_t k = n - off > (size_t)pkt_active ? (size_t)pkt_active : n - off;
        send_packet(off + k < n ? PID_DATA : PID_END, d + off, k);
        off += k;
    }
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xFF; }
static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* ---------------------------------------------------------------- power */

static void ao_stop(void) { ao.kind = AO_NONE; }

static void module_ram_reset(void) {
    img_valid = false;
    memset(img, 0, sizeof(img));
    for (int i = 1; i <= NBUF; i++) cb[i].st = CB_EMPTY;
    dl.what = 0;
    ao_stop();
    last_pass = false;
    pwd_ok = fl.pwd == 0;
    baud_active = 9600u * fl.baud_n;
    pkt_active = 32 << fl.pkt_code;
    uart_set_baudrate(UARTN, baud_active);
}

static bool power_wanted(void) {
    if (!present) return false;
    if (power_force >= 0) return power_force;
    if (PWR_GPIO < 0) return true;
    return gpio_get_level(PWR_GPIO) == PWR_ON_LEVEL;
}

static void power_on(void) {
    int64_t off_for = now_ms() - off_since_ms;
    if (off_for < MIN_OFF_MS) {
        st.fast_cycles++;
        ESP_LOGW(TAG, "powered on %lld ms after power-off (the manual asks for 2 s or more)", off_for);
    }
    vTaskDelay(pdMS_TO_TICKS(BOOT_MS));
    module_ram_reset();
    tx_attach(true);
    uart_flush_input(UARTN);
    powered = true;
    st.power_ons++;
    uart_write_bytes(UARTN, "\x55", 1);   /* "ready" byte (manual 2.4) */
    if (trace) ESP_LOGI(TAG, "power on: %lu baud, %d-byte data packets, password %s", (unsigned long)baud_active,
                        pkt_active, fl.pwd ? "set" : "not set");
}

static void power_off(void) {
    uart_wait_tx_done(UARTN, pdMS_TO_TICKS(50));
    tx_attach(false);
    powered = false;
    off_since_ms = now_ms();
    ao_stop();
    dl.what = 0;
    if (trace) ESP_LOGI(TAG, "power off");
}

/* ---------------------------------------------------------------- AutoEnroll / AutoIdentify */

static void enroll_packet(uint8_t code, uint8_t step, bool final) {
    if (!final && !ao.steps) return;
    uint8_t b[4] = { code, step, (uint8_t)(ao.id >> 8), (uint8_t)ao.id };
    cur_cmd = 0x31;
    send_reply(b, sizeof(b));
}

static void ident_packet(uint8_t code, uint8_t step, uint16_t id, uint16_t score, bool final) {
    if (!final && !ao.steps) return;
    uint8_t b[6] = { code, step, (uint8_t)(id >> 8), (uint8_t)id, (uint8_t)(score >> 8), (uint8_t)score };
    cur_cmd = 0x32;
    send_reply(b, sizeof(b));
}

static void enroll_fail(uint8_t code, uint8_t step) {
    led_auto(L_ON, COL_RED);
    enroll_packet(code, step, true);
    ao_stop();
}

static void ao_phase(ao_phase_t ph) {
    ao.phase = ph;
    ao.t0 = now_ms();
}

static void enroll_step(int64_t now) {
    uint8_t step_cap = (uint8_t)(2 * ao.capture - 1), step_gen = (uint8_t)(2 * ao.capture);
    switch (ao.phase) {
    case PH_WAIT_FINGER:
        if (finger_on && finger_q != Q_FAIL) {
            img_make(finger_on, finger_q);
            ao.fingers[ao.capture - 1] = finger_on;
            led_auto(L_ON, COL_YELLOW);
            enroll_packet(C_OK, step_cap, false);
            ao_phase(PH_GEN);
            ao.due = now + (fast ? 0 : 200);
        }
        else if (now - ao.t0 > (int64_t)opt_timeout_ms) {
            enroll_fail(C_TIMEOUT, step_cap);
        }
        break;
    case PH_GEN:
        if (now < ao.due) break;
        if (img[6] == Q_MESSY || img[6] == Q_SMALL) {
            enroll_fail(C_FEWPTS, step_gen);
            break;
        }
        led_auto(L_FLASH, COL_GREEN);
        enroll_packet(C_OK, step_gen, false);
        if (ao.capture < 6) {
            ao.capture++;
            if (ao.lift) {
                led_auto(L_ON, COL_WHITE);
                ao_phase(PH_WAIT_LIFT);
            }
            else {
                led_auto(L_FLASH, COL_BLUE);
                ao_phase(PH_WAIT_FINGER);
            }
            break;
        }
        /* 0x0D duplicate check, 0x0E merge, 0x0F store */
        if (!ao.dup_ok) {
            for (int i = 0; i < CAP; i++) {
                if (fl.finger[i] == ao.fingers[0] && !(ao.overwrite && i == ao.id)) {
                    enroll_fail(C_EXISTS, 0x0D);
                    return;
                }
            }
        }
        led_auto(L_FLASH, COL_GREEN);
        enroll_packet(C_OK, 0x0D, false);
        for (int i = 1; i < 6; i++) {
            if (ao.fingers[i] != ao.fingers[0]) {
                enroll_fail(C_MERGE, 0x0E);
                return;
            }
        }
        enroll_packet(C_OK, 0x0E, false);
        cb_set(1, ao.fingers[0], 2, 6, new_variant());
        fl.finger[ao.id] = ao.fingers[0];
        fl.meta[ao.id] = 2 << 4 | 6;
        fl.variant[ao.id] = cb[1].variant;
        flash_save();
        led_auto(L_ON, COL_GREEN);
        enroll_packet(C_OK, 0x0F, true);
        ao_stop();
        break;
    case PH_WAIT_LIFT:
        if (!finger_on) {
            led_auto(L_FLASH, COL_BLUE);
            ao_phase(PH_WAIT_FINGER);
        }
        else if (now - ao.t0 > (int64_t)opt_timeout_ms) {
            enroll_fail(C_TIMEOUT, step_cap);   /* UNVERIFIED: the manual only times out a missing finger */
        }
        break;
    default:
        break;
    }
}

static void ident_fail_attempt(uint8_t code, uint8_t step) {
    ao.errs++;
    bool final = ao.max_err && ao.errs >= ao.max_err;
    led_auto(L_ON, COL_RED);
    ident_packet(code, step, 0, 0, final);
    if (final) ao_stop();
    else ao_phase(PH_WAIT_LIFT);    /* UNVERIFIED: next attempt waits for a new touch */
}

static void ident_step(int64_t now) {
    switch (ao.phase) {
    case PH_WAIT_FINGER:
        if (finger_on && finger_q != Q_FAIL) {
            img_make(finger_on, finger_q);
            led_auto(L_ON, COL_YELLOW);
            ident_packet(C_OK, 1, 0, 0, false);
            ao_phase(PH_GEN);
            ao.due = now + (fast ? 0 : 200);
        }
        else if (now - ao.t0 > (int64_t)opt_timeout_ms) {
            led_auto(L_ON, COL_RED);
            ident_packet(C_TIMEOUT, 1, 0, 0, true);
            ao_stop();
        }
        break;
    case PH_GEN:
        if (now < ao.due) break;
        if (img[6] == Q_MESSY || img[6] == Q_SMALL) {
            ident_fail_attempt(C_FEWPTS, 2);       /* UNVERIFIED: 0x07 is not in the AutoIdentify list */
            break;
        }
        cb_set(1, (uint16_t)(img[4] << 8 | img[5]), 1, 1, new_variant());
        ident_packet(C_OK, 2, 0, 0, false);
        ao_phase(PH_SEARCH);
        ao.due = now + (fast ? 0 : 10);
        break;
    case PH_SEARCH: {
        if (now < ao.due) break;
        uint32_t end = (uint32_t)ao.start + ao.num;
        for (uint32_t i = ao.start; i < end && i < CAP; i++) {
            if (fl.finger[i] && fl.finger[i] == cb[1].finger) {
                last_pass = true;
                led_auto(L_ON, COL_GREEN);
                ident_packet(C_OK, 3, (uint16_t)i, opt_score, true);
                ao_stop();
                return;
            }
        }
        last_pass = false;
        ident_fail_attempt(C_NOTFOUND, 3);
        break;
    }
    case PH_WAIT_LIFT:
        if (!finger_on) {
            led_auto(L_BREATH, COL_WHITE);
            ao_phase(PH_WAIT_FINGER);
        }
        else if (now - ao.t0 > (int64_t)opt_timeout_ms) {
            ident_packet(C_TIMEOUT, 1, 0, 0, true);
            ao_stop();
        }
        break;
    }
}

static void ao_tick(void) {
    if (!powered) return;
    int64_t now = now_ms();
    if (ao.kind == AO_ENROLL) enroll_step(now);
    else if (ao.kind == AO_IDENT) ident_step(now);
}

/* ---------------------------------------------------------------- commands */

static bool buf_ok(uint8_t b) { return b >= 1 && b <= NBUF; }

static void do_auto_enroll(const uint8_t *p) {
    uint16_t id = get16(p + 1);
    ao = (typeof(ao)){ 0 };
    ao.overwrite = p[3]; ao.dup_ok = p[4]; ao.steps = p[5]; ao.lift = p[6];
    if (id < 1500) {
        if (id >= CAP) { ao.id = id; enroll_fail(C_RANGE, 0); return; }
        if (fl.finger[id] && !ao.overwrite) { ao.id = id; enroll_fail(C_TPLBUSY, 0); return; }
        ao.id = id;
    }
    else {                               /* 0x05DC..0xFFFF: first free slot */
        int f = -1;
        for (int i = 0; i < CAP; i++) if (!fl.finger[i]) { f = i; break; }
        if (f < 0) { enroll_fail(C_FULL, 0); return; }   /* UNVERIFIED: the chart says 0x0B */
        ao.id = (uint16_t)f;
    }
    ao.kind = AO_ENROLL;
    ao.capture = 1;
    led_auto(L_FLASH, COL_BLUE);
    ao_phase(PH_WAIT_FINGER);
}

static void do_auto_identify(const uint8_t *p) {
    ao = (typeof(ao)){ 0 };
    ao.grade = p[1]; ao.start = get16(p + 2); ao.num = get16(p + 4); ao.steps = p[6]; ao.max_err = p[7];
    if (ao.grade < 1 || ao.grade > 5) { ident_packet(C_PKT, 0, 0, 0, true); return; }
    if (ao.start >= CAP || ao.num == 0 || (uint32_t)ao.start + ao.num > CAP) {
        ident_packet(C_RANGE, 0, 0, 0, true);
        return;
    }
    if (!lib_count()) { ident_packet(C_DBEMPTY, 0, 0, 0, true); return; }
    ao.kind = AO_IDENT;
    led_auto(L_BREATH, COL_WHITE);
    ao_phase(PH_WAIT_FINGER);
}

/* Payload length (command code included) of each command */
static int cmd_len(uint8_t c) {
    switch (c) {
    case 0x01: case 0x03: case 0x05: case 0x0A: case 0x0B: case 0x0D: case 0x0F: case 0x14:
    case 0x16: case 0x1D: case 0x30: case 0x39: case 0x3A: case 0x40:
        return 1;
    case 0x02: case 0x08: case 0x09: case 0x19: case 0x1F: return 2;
    case 0x0E: return 3;
    case 0x06: case 0x07: return 4;
    case 0x0C: case 0x12: case 0x13: case 0x15: case 0x35: return 5;
    case 0x04: return 6;
    case 0x31: return 7;
    case 0x32: return 8;
    case 0x18: return 34;
    default: return -1;
    }
}

static void handle_command(const uint8_t *p, size_t n) {
    uint8_t c = p[0];
    uint8_t out[40];
    cur_cmd = c;
    st.cmds++;
    if (trace) ESP_LOGI(TAG, "%s (0x%02X)", cmd_name(c), c);

    if (ao.kind != AO_NONE && c != 0x30) {
        st.busy_drop++;
        ESP_LOGW(TAG, "%s ignored: %s in progress", cmd_name(c), ao.kind == AO_ENROLL ? "AutoEnroll" : "AutoIdentify");
        return;                         /* UNVERIFIED: the manual does not say what happens */
    }
    int want = cmd_len(c);
    if (want < 0) { st.unsupported++; reply(C_UNSUPP); return; }
    if ((int)n != want) {
        st.bad_len++;
        if (c == 0x31 || c == 0x32) return;   /* charts 6.3/6.4: malformed -> no answer */
        reply(C_PKT);
        return;
    }
    /* Real R502-F Pro: a password set gates nothing (every command answers after a power cut,
     * 2026-11-13). fp opt pwdgate 1 emulates a module that would require VfyPwd first. */
    if (opt_pwdgate && !pwd_ok && c != 0x13) { reply(C_NEEDPWD); return; }

    switch (c) {
    case 0x01:                                                    /* GetImage */
        if (!finger_on) { reply(C_NOFINGER); break; }
        work_delay(100);
        if (finger_q == Q_FAIL) { img_valid = false; reply(C_IMGFAIL); break; }
        img_make(finger_on, finger_q);
        reply(C_OK);
        break;

    case 0x02: {                                                  /* GenChar */
        uint16_t f; uint8_t q;
        if (!buf_ok(p[1])) { reply(C_PKT); break; }               /* UNVERIFIED */
        if (!img_valid) { reply(C_NOIMG); break; }
        work_delay(200);
        if (!img_parse(&f, &q) || q == Q_MESSY) { reply(C_MESSY); break; }
        if (q == Q_SMALL) { reply(C_FEWPTS); break; }
        cb_set(p[1], f, 1, 1, new_variant());   /* every capture differs */
        reply(C_OK);
        break;
    }

    case 0x03: {                                                  /* Match: CharBuffer 1 vs 2 */
        /* Real R502-F Pro (2026-11-13): a template against itself 409, always; two merged templates
         * of the same finger (different enrolments) ~0, no match; a fresh capture against a
         * template of its finger matches (opt score). */
        bool both = cb[1].st == CB_VALID && cb[2].st == CB_VALID;
        bool same_tpl = both && cb[1].finger == cb[2].finger && cb[1].kind == cb[2].kind &&
                        cb[1].count == cb[2].count && cb[1].variant == cb[2].variant;
        bool m = same_tpl || (both && cb[1].finger == cb[2].finger && (cb[1].kind == 1 || cb[2].kind == 1));
        last_pass = m;
        put16(out, same_tpl ? SCORE_SAME : m ? opt_score : 0);
        reply_with(m ? C_OK : C_NOMATCH, out, 2);
        break;
    }

    case 0x04: {                                                  /* Search */
        uint16_t start = get16(p + 2), num = get16(p + 4);
        if (!buf_ok(p[1])) { reply(C_PKT); break; }
        if (start >= CAP) { put16(out, 0); put16(out + 2, 0); reply_with(C_RANGE, out, 4); break; }   /* UNVERIFIED */
        work_delay(10);
        int found = -1;
        if (cb[p[1]].st == CB_VALID) {
            for (uint32_t i = start; i < (uint32_t)start + num && i < CAP; i++)
                if (fl.finger[i] && fl.finger[i] == cb[p[1]].finger) { found = (int)i; break; }
        }
        last_pass = found >= 0;
        put16(out, found >= 0 ? (uint16_t)found : 0);
        put16(out + 2, found >= 0 ? opt_score : 0);
        reply_with(found >= 0 ? C_OK : C_NOTFOUND, out, 4);
        break;
    }

    case 0x05: {                                                  /* RegModel */
        int nfeat = 0;
        uint16_t f = 0;
        bool same = true;
        for (int b = 1; b <= NBUF; b++) {
            if (cb[b].st != CB_VALID || cb[b].kind != 1) continue;   /* UNVERIFIED: which buffers */
            if (!nfeat) f = cb[b].finger;
            else if (cb[b].finger != f) same = false;
            nfeat++;
        }
        work_delay(50);
        if (nfeat < 2 || !same) { reply(C_MERGE); break; }
        cb_set(1, f, 2, (uint8_t)nfeat, new_variant());
        for (int b = 2; b <= NBUF; b++) cb[b].st = CB_EMPTY;
        reply(C_OK);
        break;
    }

    case 0x06: {                                                  /* StoreChar */
        uint16_t id = get16(p + 2);
        if (!buf_ok(p[1])) { reply(C_PKT); break; }
        if (id >= CAP) { reply(C_RANGE); break; }
        if (cb[p[1]].st != CB_VALID) { reply(C_BADDATA); break; }  /* UNVERIFIED */
        fl.finger[id] = cb[p[1]].finger;
        fl.meta[id] = (uint8_t)(cb[p[1]].kind << 4 | (cb[p[1]].count > 15 ? 15 : cb[p[1]].count));
        fl.variant[id] = cb[p[1]].variant;      /* stored re-encoded: LoadChar gives the canonical bytes */
        flash_save();
        work_delay(20);
        reply(C_OK);
        break;
    }

    case 0x07: {                                                  /* LoadChar */
        uint16_t id = get16(p + 2);
        if (!buf_ok(p[1])) { reply(C_PKT); break; }
        if (id >= CAP) { reply(C_RANGE); break; }
        if (!fl.finger[id]) { reply(C_LOADBAD); break; }
        cb_set(p[1], fl.finger[id], fl.meta[id] >> 4, fl.meta[id] & 0xF, fl.variant[id]);
        reply(C_OK);
        break;
    }

    case 0x08: {                                                  /* UpChar */
        int b = buf_ok(p[1]) ? p[1] : 1;                          /* UNVERIFIED: manual says the id is unused */
        if (cb[b].st == CB_EMPTY) { reply(C_UPCHAR); break; }
        if (reply(C_OK)) send_data(cb[b].raw, TPL_BYTES);
        break;
    }

    case 0x09: {                                                  /* DownChar */
        int b = buf_ok(p[1]) ? p[1] : 1;
        if (reply(C_OK)) dl = (typeof(dl)){ 'c', b, 0, false };
        break;
    }

    case 0x0A:                                                    /* UpImage */
        if (!img_valid) { reply(C_NOTX); break; }
        if (reply(C_OK)) send_data(img, IMG_BYTES);
        break;

    case 0x0B:                                                    /* DownImage */
        if (reply(C_OK)) dl = (typeof(dl)){ 'i', 0, 0, false };
        break;

    case 0x0C: {                                                  /* DeleteChar */
        uint16_t start = get16(p + 1), num = get16(p + 3);
        if (num == 0 || start >= CAP || (uint32_t)start + num > CAP) { reply(C_DELFAIL); break; }
        for (uint32_t i = start; i < (uint32_t)start + num; i++) { fl.finger[i] = 0; fl.meta[i] = 0; fl.variant[i] = 0; }
        flash_save();
        reply(C_OK);
        break;
    }

    case 0x0D:                                                    /* Empty */
        memset(fl.finger, 0, sizeof(fl.finger));
        memset(fl.meta, 0, sizeof(fl.meta));
        memset(fl.variant, 0, sizeof(fl.variant));
        flash_save();
        work_delay(50);
        reply(C_OK);
        break;

    case 0x0E: {                                                  /* SetSysPara */
        uint8_t idx = p[1], v = p[2];
        bool ok;
        if (idx == 4) ok = v == 1 || v == 2 || v == 4 || v == 6 || v == 12;
        else if (idx == 5) ok = v >= 1 && v <= 5;
        else if (idx == 6) ok = v <= 3;
        else { reply(C_BADREG); break; }
        if (!ok) { reply(C_BADREG); break; }                     /* UNVERIFIED: code for a bad value */
        if (idx == 4) fl.baud_n = v;
        else if (idx == 5) fl.security = v;
        else fl.pkt_code = v;
        flash_save();
        reply(C_OK);
        if (idx == 4) {              /* real module: the baud rate changes at once, after the answer */
            uart_wait_tx_done(UARTN, pdMS_TO_TICKS(100));
            baud_active = 9600u * fl.baud_n;
            uart_set_baudrate(UARTN, baud_active);
        }                            /* UNVERIFIED: packet size and security level at the next power-on */
        break;
    }

    case 0x0F: {                                                  /* ReadSysPara */
        uint16_t status = (uint16_t)((last_pass ? 2 : 0) | (pwd_ok ? 4 : 0) | (img_valid ? 8 : 0));
        put16(out, status);              /* bits from older GROW manuals: Busy, Pass, PWD, ImgBufStat */
        put16(out + 2, 600);             /* system identifier of the real R502-F Pro */
        put16(out + 4, CAP);
        put16(out + 6, fl.security);
        put32(out + 8, fl.addr);
        put16(out + 12, fl.pkt_code);
        put16(out + 14, fl.baud_n);
        reply_with(C_OK, out, 16);
        break;
    }

    case 0x12:                                                    /* SetPwd */
        fl.pwd = get32(p + 1);
        flash_save();
        pwd_ok = true;
        reply(C_OK);
        break;

    case 0x13:                                                    /* VfyPwd */
        if (get32(p + 1) == fl.pwd) { pwd_ok = true; reply(C_OK); }
        else { st.bad_pwd++; reply(C_BADPWD); }
        break;

    case 0x14:                                                    /* GetRandomCode */
        put32(out, esp_random());
        reply_with(C_OK, out, 4);
        break;

    case 0x15:                                                    /* SetAddr */
        reply(C_OK);                     /* UNVERIFIED: answered from the old address */
        fl.addr = get32(p + 1);
        flash_save();
        break;

    case 0x16: {                                                  /* ReadInfPage */
        static uint8_t page[INFO_BYTES];
        memset(page, 0, sizeof(page));
        snprintf((char *)page, sizeof(page),
                 "fp_emu: GROW R502F-Pro emulator (cardea-lab-bench), not a real module. "
                 "Library %d, image %dx%d, template %d bytes.", CAP, IMG_W, IMG_H, TPL_BYTES);
        if (reply(C_OK)) send_data(page, sizeof(page));
        break;
    }

    case 0x18:                                                    /* WriteNotepad */
        if (p[1] > 15) { reply(C_BADREG); break; }                /* UNVERIFIED */
        memcpy(fl.notepad[p[1]], p + 2, 32);
        flash_save();
        reply(C_OK);
        break;

    case 0x19:                                                    /* ReadNotepad */
        if (p[1] > 15) { reply(C_BADREG); break; }
        reply_with(C_OK, fl.notepad[p[1]], 32);
        break;

    case 0x1D:                                                    /* ValidTemplateNum */
        put16(out, (uint16_t)lib_count());
        reply_with(C_OK, out, 2);
        break;

    case 0x1F: {                                                  /* ReadIndexTable */
        if (p[1] > 5) { reply(C_BADREG); break; }                 /* UNVERIFIED */
        memset(out, 0, 32);
        for (int k = 0; k < 256; k++) {
            int slot = p[1] * 256 + k;
            if (slot < CAP && fl.finger[slot]) out[k / 8] |= (uint8_t)(1 << (k % 8));
        }
        reply_with(C_OK, out, 32);
        break;
    }

    case 0x30:                                                    /* Cancel */
        if (ao.kind != AO_NONE) led_auto(L_OFF, 0);
        ao_stop();
        reply(C_OK);                     /* UNVERIFIED: no closing packet from the cancelled operation */
        break;

    case 0x31: do_auto_enroll(p); break;
    case 0x32: do_auto_identify(p); break;

    case 0x35:                                                    /* AuraLedConfig */
        led_record('h', p[1], p[2], p[3], p[4]);
        if (p[1] < 1 || p[1] > 6) ESP_LOGW(TAG, "AuraLedConfig: control code 0x%02X not in the manual", p[1]);
        reply(C_OK);
        break;

    case 0x39:                                                    /* GetAlgVer */
    case 0x3A: {                                                  /* GetFwVer */
        memset(out, 0, 32);
        snprintf((char *)out, 32, "%s", c == 0x39 ? "fp_emu-alg-1" : "fp_emu-R502F-Pro-1");
        reply_with(C_OK, out, 32);
        break;
    }

    case 0x40:                                                    /* HandShake */
        reply(C_OK);
        break;
    }
}

static void handle_data(uint8_t pid, const uint8_t *d, size_t n) {
    if (!dl.what) {
        if (trace) ESP_LOGW(TAG, "data packet with no transfer open: dropped");
        return;
    }
    uint8_t *dst = dl.what == 'c' ? dl_tpl : img;
    size_t max = dl.what == 'c' ? TPL_BYTES : IMG_BYTES;
    if (dl.got + n > max) dl.overflow = true;
    else memcpy(dst + dl.got, d, n);
    dl.got += n;
    if (pid != PID_END) return;

    bool complete = !dl.overflow && dl.got == max;
    if (dl.what == 'c') {
        /* Real R502-F Pro (2026-11-13): data that is not a template is dropped in silence (DownChar
         * answered 0x00) and the buffer keeps what it held; a template is taken as sent. */
        charbuf_t *b = &cb[dl.buf];
        uint16_t f; uint8_t k, n2, v;
        bool ok = complete && tpl_parse(dl_tpl, &f, &k, &n2, &v);
        if (ok) {
            memcpy(b->raw, dl_tpl, TPL_BYTES);
            b->st = CB_VALID; b->finger = f; b->kind = k; b->count = n2; b->variant = v;
        }
        if (trace) ESP_LOGI(TAG, "DownChar: %u bytes for buffer %d (%s)", (unsigned)dl.got, dl.buf,
                            ok ? "template taken" : complete ? "not a template: dropped, buffer unchanged" : "wrong size: dropped");
    }
    else {
        img_valid = complete;
        if (trace) ESP_LOGI(TAG, "DownImage: %u bytes (%s)", (unsigned)dl.got, complete ? "ok" : "wrong size");
    }
    dl.what = 0;
}

/* ---------------------------------------------------------------- frame parser */

static struct {
    int state;               /* 0..1 header, 2..5 address, 6 pid, 7..8 length, 9 body */
    uint8_t addr[4], pid;
    uint16_t len, got;
    uint8_t body[MAX_BODY];
    int64_t last_ms;
} rx;

static void frame_done(void) {
    uint8_t hdr[3] = { rx.pid, (uint8_t)(rx.len >> 8), (uint8_t)rx.len };
    uint16_t s = (uint16_t)(sum16(hdr, 3) + sum16(rx.body, rx.len - 2));
    uint16_t got = (uint16_t)(rx.body[rx.len - 2] << 8 | rx.body[rx.len - 1]);
    size_t n = rx.len - 2;
    if (get32(rx.addr) != fl.addr) {
        st.other_addr++;
        if (trace) ESP_LOGW(TAG, "packet for address %08lX ignored", (unsigned long)get32(rx.addr));
        return;                          /* UNVERIFIED: silence rather than 0x20 */
    }
    if (s != got) {
        st.bad_sum++;
        ESP_LOGW(TAG, "bad checksum (pid 0x%02X)", rx.pid);
        if (rx.pid == PID_CMD) { cur_cmd = n ? rx.body[0] : 0; reply(C_PKT); }
        else if (dl.what) { dl.what = 0; ESP_LOGW(TAG, "download aborted"); }
        return;
    }
    if (rx.pid == PID_CMD) {
        if (dl.what) { dl.what = 0; ESP_LOGW(TAG, "command in the middle of a download: download aborted"); }
        if (n) handle_command(rx.body, n);
    }
    else if (rx.pid == PID_DATA || rx.pid == PID_END) {
        handle_data(rx.pid, rx.body, n);
    }
}

static void rx_byte(uint8_t b) {
    int64_t now = now_ms();
    if (rx.state && now - rx.last_ms > FRAME_GAP_MS) rx.state = 0;
    rx.last_ms = now;
    switch (rx.state) {
    case 0: if (b == 0xEF) rx.state = 1; break;
    case 1: rx.state = b == 0x01 ? 2 : (b == 0xEF ? 1 : 0); break;
    case 2: case 3: case 4: case 5: rx.addr[rx.state - 2] = b; rx.state++; break;
    case 6: rx.pid = b; rx.state = 7; break;
    case 7: rx.len = (uint16_t)(b << 8); rx.state = 8; break;
    case 8:
        rx.len |= b;
        rx.got = 0;
        rx.state = (rx.len >= 3 && rx.len <= MAX_BODY) ? 9 : 0;
        break;
    case 9:
        rx.body[rx.got++] = b;
        if (rx.got == rx.len) {
            rx.state = 0;
            frame_done();
        }
        break;
    }
}

/* ---------------------------------------------------------------- task */

static void fp_task(void *arg) {
    (void)arg;
    uint8_t buf[128];
    for (;;) {
        bool want = power_wanted();
        xSemaphoreTake(mx, portMAX_DELAY);
        if (want && !powered) power_on();
        else if (!want && powered) power_off();
        xSemaphoreGive(mx);

        if (!powered) {
            uart_flush_input(UARTN);
            rx.state = 0;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int n = uart_read_bytes(UARTN, buf, sizeof(buf), pdMS_TO_TICKS(10));
        xSemaphoreTake(mx, portMAX_DELAY);
        if (n > 0) {
            hexlog("rx", buf, (size_t)n);
            for (int i = 0; i < n && powered; i++) rx_byte(buf[i]);
        }
        ao_tick();
        xSemaphoreGive(mx);
    }
}

/* Background touches ("fp tap") */
typedef struct { uint16_t finger; uint8_t q; int n, on_ms, off_ms; } tap_t;
static QueueHandle_t tapq;
static volatile uint32_t tap_gen;        /* bumped by "fp finger": stops a running tap sequence */

static void set_finger(uint16_t f, uint8_t q) {
    finger_q = q;
    finger_on = f;
    wake_update();
}

static void tap_task(void *arg) {
    (void)arg;
    tap_t t;
    for (;;) {
        if (xQueueReceive(tapq, &t, portMAX_DELAY) != pdTRUE) continue;
        uint32_t gen = tap_gen;
        for (int i = 0; i < t.n && gen == tap_gen; i++) {
            set_finger(t.finger, t.q);
            vTaskDelay(pdMS_TO_TICKS(t.on_ms));
            if (gen != tap_gen) break;
            set_finger(0, Q_GOOD);
            if (i + 1 < t.n) vTaskDelay(pdMS_TO_TICKS(t.off_ms));
            if (uxQueueMessagesWaiting(tapq)) break;   /* a newer request replaces this one */
        }
    }
}

/* ---------------------------------------------------------------- console */

static char *next_tok(char **s) {
    return strtok_r(NULL, " \t", s);
}

static void fp_status(void) {
    char f[8];
    fname(finger_on, f, sizeof(f));
    const char *busy = ao.kind == AO_ENROLL ? "enroll" : ao.kind == AO_IDENT ? "identify" : "none";
    lab_console_reply("OK fp status pwr=%d(%s) present=%d finger=%s%s%s wake=%d baud=%lu pkt=%d addr=%08lX "
                      "pwd=%s tpl=%d busy=%s speed=%s",
                      powered, power_force < 0 ? "pin" : "forced", present, finger_on ? f : "off",
                      finger_on ? "/" : "", finger_on ? q_names[finger_q] : "",
                      WAKE_GPIO >= 0 && present ? gpio_get_level(WAKE_GPIO) : -1, (unsigned long)baud_active,
                      pkt_active, (unsigned long)fl.addr, fl.pwd ? (pwd_ok ? "set,ok" : "set") : "none",
                      lib_count(), busy, fast ? "fast" : "real");
}

static void fp_next(char **sp) {
    char *a = next_tok(sp);
    if (!a) { lab_console_reply("OK fp next queued=%d", n_faults); return; }
    if (!strcasecmp(a, "clear")) { n_faults = 0; lab_console_reply("OK fp next cleared"); return; }
    int cmd = cmd_by_name(a);
    char *k = next_tok(sp), *v = next_tok(sp);
    if (cmd == -2 || !k) { lab_console_reply("ERR fp next <cmd|any> <code XX|silent|badsum|short|garbage|delay MS|reboot|page N>"); return; }
    fault_t f = { cmd, F_CODE, 0 };
    int i;
    for (i = 0; i < N_FAULT_KINDS; i++) if (!strcasecmp(k, fault_names[i])) break;
    if (i == N_FAULT_KINDS) { lab_console_reply("ERR fp next: unknown fault '%s'", k); return; }
    f.k = (fault_kind_t)i;
    if (f.k == F_CODE || f.k == F_DELAY || f.k == F_PAGE) {
        if (!v) { lab_console_reply("ERR fp next: %s needs a value", k); return; }
        f.arg = (int)strtol(v, NULL, f.k == F_CODE ? 16 : 10);
        if (f.k == F_CODE && (f.arg < 0 || f.arg > 0xFF)) { lab_console_reply("ERR fp next: code 00..FF"); return; }
        if (f.k == F_PAGE && (f.arg < 0 || f.arg > 0xFFFF)) { lab_console_reply("ERR fp next: page 0..65535"); return; }
        if (f.k == F_DELAY && (f.arg < 1 || f.arg > 30000)) { lab_console_reply("ERR fp next: delay 1..30000 ms"); return; }
    }
    if (n_faults >= MAX_FAULTS) { lab_console_reply("ERR fp next: queue full (%d)", MAX_FAULTS); return; }
    faults[n_faults++] = f;
    lab_console_reply("OK fp next %s %s %d (queued=%d)", cmd < 0 ? "any" : cmd_name((uint8_t)cmd), fault_names[f.k],
                      f.arg, n_faults);
}

static void fp_led(char **sp) {
    char *a = next_tok(sp);
    if (a && !strcasecmp(a, "clear")) { led_n = 0; lab_console_reply("OK fp led cleared"); return; }
    char line[150];
    size_t k = 0;
    line[0] = 0;
    for (uint32_t i = 0; i < 4 && i < led_n && i < LED_LOG; i++) {
        const led_ev_t *e = &led_log[(led_n - 1 - i) % LED_LOG];
        k += snprintf(line + k, sizeof(line) - k, " %s:%s/%u/%s/%u", e->src == 'h' ? "host" : "module",
                      led_ctrl_name(e->ctrl), e->speed, led_color_name(e->color), e->times);
        if (k >= sizeof(line)) break;
    }
    lab_console_reply("OK fp led n=%lu%s", (unsigned long)led_n, line);
}

static void fp_db(char **sp) {
    char *a = next_tok(sp);
    if (a && (!strcasecmp(a, "set") || !strcasecmp(a, "del"))) {
        char *s = next_tok(sp), *fs = next_tok(sp);
        long slot = s ? strtol(s, NULL, 10) : -1;
        if (slot < 0 || slot >= CAP) { lab_console_reply("ERR fp db: slot 0..%d", CAP - 1); return; }
        if (!strcasecmp(a, "set")) {
            int f = parse_finger(fs);
            if (f < 0) { lab_console_reply("ERR fp db set <slot> <finger>"); return; }
            fl.finger[slot] = (uint16_t)f;
            fl.meta[slot] = 2 << 4 | 6;
            fl.variant[slot] = new_variant();
        }
        else {
            fl.finger[slot] = 0;
            fl.meta[slot] = 0;
        }
        flash_save();
        lab_console_reply("OK fp db %s %ld", a, slot);
        return;
    }
    char line[140];
    size_t k = 0;
    line[0] = 0;
    for (int i = 0; i < CAP && k < sizeof(line) - 16; i++) {
        if (!fl.finger[i]) continue;
        char f[8];
        fname(fl.finger[i], f, sizeof(f));
        k += snprintf(line + k, sizeof(line) - k, " %d:%s", i, f);
    }
    if (k >= sizeof(line) - 16) snprintf(line + k, sizeof(line) - k, " ...");
    lab_console_reply("OK fp db n=%d%s", lib_count(), line);
}

static void fp_opt(char **sp) {
    char *k = next_tok(sp), *v = next_tok(sp);
    if (!k || !v) { lab_console_reply("ERR fp opt speed fast|real | score N | timeout MS | trace 0|1|2 | pwdgate 0|1"); return; }
    if (!strcasecmp(k, "speed")) fast = !strcasecmp(v, "fast");
    else if (!strcasecmp(k, "score")) opt_score = (uint16_t)strtol(v, NULL, 10);
    else if (!strcasecmp(k, "timeout")) opt_timeout_ms = (uint32_t)strtol(v, NULL, 10);
    else if (!strcasecmp(k, "trace")) trace = (int)strtol(v, NULL, 10);
    else if (!strcasecmp(k, "pwdgate")) opt_pwdgate = strtol(v, NULL, 10) != 0;
    else { lab_console_reply("ERR fp opt: unknown option '%s'", k); return; }
    lab_console_reply("OK fp opt speed=%s score=%u timeout=%lu trace=%d pwdgate=%d", fast ? "fast" : "real", opt_score,
                      (unsigned long)opt_timeout_ms, trace, opt_pwdgate);
}

static void cmd_fp(char *arg) {
    char *sp = NULL;
    char *sub = arg ? strtok_r(arg, " \t", &sp) : NULL;
    if (!sub) { lab_console_reply("ERR fp status|finger|lift|tap|power|unplug|plug|next|led|db|factory|opt|stats"); return; }

    if (!strcasecmp(sub, "status")) {
        xSemaphoreTake(mx, portMAX_DELAY);
        fp_status();
        xSemaphoreGive(mx);
    }
    else if (!strcasecmp(sub, "finger") || !strcasecmp(sub, "lift")) {
        const char *fs = strcasecmp(sub, "lift") ? next_tok(&sp) : "off";
        tap_gen++;
        if (fs && !strcasecmp(fs, "off")) {
            xQueueReset(tapq);
            set_finger(0, Q_GOOD);
            lab_console_reply("OK fp finger off");
            return;
        }
        int f = parse_finger(fs);
        int q = parse_quality(next_tok(&sp));
        if (f < 0 || q < 0) { lab_console_reply("ERR fp finger <A..Z|1..65534|off> [good|messy|small|fail]"); return; }
        set_finger((uint16_t)f, (uint8_t)q);
        lab_console_reply("OK fp finger %s %s", fs, q_names[q]);
    }
    else if (!strcasecmp(sub, "tap")) {
        char *fs = next_tok(&sp), *ns = next_tok(&sp), *on = next_tok(&sp), *off = next_tok(&sp);
        int f = parse_finger(fs), q = parse_quality(next_tok(&sp));
        tap_t t = { (uint16_t)f, (uint8_t)q, ns ? atoi(ns) : 1, on ? atoi(on) : 600, off ? atoi(off) : 600 };
        if (f < 0 || q < 0 || t.n < 1 || t.n > 100 || t.on_ms < 10 || t.off_ms < 10) {
            lab_console_reply("ERR fp tap <finger> [n 1..100] [on_ms] [off_ms] [quality]");
            return;
        }
        tap_gen++;
        xQueueOverwrite(tapq, &t);
        lab_console_reply("OK fp tap %s x%d (%d/%d ms)", fs, t.n, t.on_ms, t.off_ms);
    }
    else if (!strcasecmp(sub, "power")) {
        char *v = next_tok(&sp);
        if (v && !strcasecmp(v, "on")) power_force = 1;
        else if (v && !strcasecmp(v, "off")) power_force = 0;
        else if (v && !strcasecmp(v, "pin")) power_force = -1;
        else if (v) { lab_console_reply("ERR fp power [on|off|pin]"); return; }
        lab_console_reply("OK fp power mode=%s line=%d", power_force < 0 ? "pin" : power_force ? "on" : "off",
                          PWR_GPIO >= 0 ? gpio_get_level(PWR_GPIO) : -1);
    }
    else if (!strcasecmp(sub, "unplug") || !strcasecmp(sub, "plug")) {
        present = !strcasecmp(sub, "plug");
        wake_update();
        lab_console_reply("OK fp %s", sub);
    }
    else if (!strcasecmp(sub, "next")) {
        xSemaphoreTake(mx, portMAX_DELAY);
        fp_next(&sp);
        xSemaphoreGive(mx);
    }
    else if (!strcasecmp(sub, "led")) {
        fp_led(&sp);
    }
    else if (!strcasecmp(sub, "db")) {
        xSemaphoreTake(mx, portMAX_DELAY);
        fp_db(&sp);
        xSemaphoreGive(mx);
    }
    else if (!strcasecmp(sub, "factory")) {
        xSemaphoreTake(mx, portMAX_DELAY);
        flash_defaults();
        flash_save();
        if (powered) module_ram_reset();
        xSemaphoreGive(mx);
        lab_console_reply("OK fp factory (library, password, address, parameters, notepad erased)");
    }
    else if (!strcasecmp(sub, "opt")) {
        fp_opt(&sp);
    }
    else if (!strcasecmp(sub, "stats")) {
        char *a = next_tok(&sp);
        if (a && !strcasecmp(a, "clear")) { memset(&st, 0, sizeof(st)); lab_console_reply("OK fp stats cleared"); return; }
        lab_console_reply("OK fp stats cmds=%lu badsum=%lu otheraddr=%lu badlen=%lu badpwd=%lu unsupported=%lu "
                          "busy=%lu poweron=%lu fastcycle=%lu faults=%lu",
                          (unsigned long)st.cmds, (unsigned long)st.bad_sum, (unsigned long)st.other_addr,
                          (unsigned long)st.bad_len, (unsigned long)st.bad_pwd, (unsigned long)st.unsupported,
                          (unsigned long)st.busy_drop, (unsigned long)st.power_ons, (unsigned long)st.fast_cycles,
                          (unsigned long)st.faults);
    }
    else {
        lab_console_reply("ERR fp: unknown subcommand '%s'", sub);
    }
}

/* ---------------------------------------------------------------- start */

void fp_emu_start(void) {
    mx = xSemaphoreCreateMutex();
    tapq = xQueueCreate(1, sizeof(tap_t));
    flash_load();

    const uart_config_t uc = {
        .baud_rate = 9600 * fl.baud_n,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UARTN, 1024, 2048, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UARTN, &uc));
    ESP_ERROR_CHECK(uart_set_pin(UARTN, TX_GPIO, RX_GPIO, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    gpio_pullup_en(RX_GPIO);             /* host TX not driven (host in reset): idle, not noise */
    tx_attach(false);

    if (WAKE_GPIO >= 0) {
        gpio_reset_pin(WAKE_GPIO);
        gpio_set_pull_mode(WAKE_GPIO, GPIO_FLOATING);
        wake_update();
    }
    if (PWR_GPIO >= 0) {
        gpio_config_t c = {
            .pin_bit_mask = 1ULL << PWR_GPIO,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = PWR_ON_LEVEL == 0 ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
            .pull_down_en = PWR_ON_LEVEL == 0 ? GPIO_PULLDOWN_DISABLE : GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&c);
    }

    lab_console_register("fp", "fp <sub>", cmd_fp);
    xTaskCreate(fp_task, "fp_emu", 4096, NULL, 5, NULL);
    xTaskCreate(tap_task, "fp_tap", 2048, NULL, 3, NULL);
    ESP_LOGI(TAG, "R502-F Pro emulator: UART%d TX GPIO%d RX GPIO%d, WAKEUP GPIO%d, power GPIO%d; library %d/%d",
             UARTN, TX_GPIO, RX_GPIO, WAKE_GPIO, PWR_GPIO, lib_count(), CAP);
}
