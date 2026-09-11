/* ============================================================================
 *  ED060KD1 (1448 x 1072) e-paper HOME SCREEN UI  --  epdiy V7 / ESP32-S3
 *
 *  A Chinese ink-wash style home screen, driven by three buttons that share
 *  one ADC line (GPIO19).  The three buttons are classified by fixed raw ADC
 *  voltage windows (KEY1 / KEY2 / KEY3 / NONE).
 *
 *  Flash setup: use Partition Scheme "8M with spiffs (3MB APP/1.5MB SPIFFS)".
 * ==========================================================================*/

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_types.h>
#include <assert.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <driver/gpio.h>

#include <epdiy.h>

#include "sdkconfig.h"

/* Subset Chinese fonts (generated with scripts/fontconvert.py). */
#include "ui_sc_20.h"
#include "ui_clock.h"
#include "img_bg.h"

/* ------------------------------------------------------------------------- */
/*  ADC entry points (Arduino.h is C++, declare analogRead from C)            */
/* ------------------------------------------------------------------------- */
#ifdef ARDUINO_ARCH_ESP32
extern uint16_t analogRead(uint8_t pin);
extern void analogReadResolution(uint8_t bits);
#define BTN_ADC_PIN 19
#else
#include <driver/adc.h>
/* GPIO19 is ADC2 channel 8 on the ESP32-S3. */
#define BTN_ADC_CHANNEL ADC2_CHANNEL_8
#define BTN_ADC_PIN 19
#endif

#define WAVEFORM EPD_BUILTIN_WAVEFORM

/* ------------------------------------------------------------------------- */
/*  Hardware configuration                                                    */
/* ------------------------------------------------------------------------- */

/* E-paper power switch (active high). */
#define EPD_PWR_PIN GPIO_NUM_46

static inline void epd_power_enable(void) { gpio_set_level(EPD_PWR_PIN, 1); }
static inline void epd_power_disable(void) { gpio_set_level(EPD_PWR_PIN, 0); }

/* choose the default demo board depending on the architecture */
#ifdef CONFIG_IDF_TARGET_ESP32
#define DEMO_BOARD epd_board_v6
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
#define DEMO_BOARD epd_board_v7
#endif

/* ------------------------------------------------------------------------- */
/*  Display definition                                                        */
/* ------------------------------------------------------------------------- */

EpdiyHighlevelState hl;

const EpdDisplay_t ED060KD1 = {
    .width = 1448,
    .height = 1072,
    .bus_width = 8,
    .bus_speed = 20,
    .default_waveform = &epdiy_ED060SCT,
    .display_type = DISPLAY_TYPE_GENERIC,
};

/* ------------------------------------------------------------------------- */
/*  Colors (4 bit grayscale, 0 = black, 15 = white)                           */
/* ------------------------------------------------------------------------- */

#define G_WHITE 15
#define G_LGRAY 12
#define G_GRAY 8
#define G_DGRAY 4
#define G_BLACK 0
#define PX(g) ((uint8_t)((g) << 4))

/* ------------------------------------------------------------------------- */
/*  Global state                                                              */
/* ------------------------------------------------------------------------- */

static uint8_t* fb = NULL;
static uint8_t* g_static_fb = NULL; /* background + static content, no highlight */
static const EpdFont* font_body = &UiSC20;
static const EpdFont* font_clock = &UiClock;

static int SW = 0; /* rotated display width  */
static int SH = 0; /* rotated display height */
static int g_temperature = 22;

/* ------------------------------------------------------------------------- */
/*  Buttons: voltage ladder on one ADC pin (no calibration)                   */
/* ------------------------------------------------------------------------- */

enum {
    BTN_NONE = -1,
    BTN_KEY1 = 0,
    BTN_KEY2 = 1,
    BTN_KEY3 = 2,
};
#define BTN_COUNT 3
#define BTN_UP BTN_KEY1
#define BTN_OK BTN_KEY2
#define BTN_DOWN BTN_KEY3

/* Key windows in RAW 12-bit ADC counts (0..4095). */
#define KEY1_MIN_RAW 2150 /* D1 = 2310..2699 */
#define KEY1_MAX_RAW 2950
#define KEY2_MIN_RAW 1400 /* D2 = 1666..1933 */
#define KEY2_MAX_RAW 2050
#define KEY3_MIN_RAW 650 /* D3 = 825..~960 */
#define KEY3_MAX_RAW 1250

#define KEY_HYST_RAW 40
#define BTN_STABLE_SAMPLES 3
#define BTN_POLL_MS 20

typedef struct {
    uint16_t min_raw;
    uint16_t max_raw;
} key_window_t;

static const key_window_t key_windows[BTN_COUNT] = {
    {KEY1_MIN_RAW, KEY1_MAX_RAW},
    {KEY2_MIN_RAW, KEY2_MAX_RAW},
    {KEY3_MIN_RAW, KEY3_MAX_RAW},
};

static int s_btn_state = BTN_NONE;
static int s_btn_candidate = BTN_NONE;
static int s_btn_candidate_count = 0;

static inline int iabs(int v) { return v < 0 ? -v : v; }

static uint16_t btn_read_raw(void) {
#ifdef ARDUINO_ARCH_ESP32
    return analogRead(BTN_ADC_PIN);
#else
    int raw = 0;
    if (adc2_get_raw(BTN_ADC_CHANNEL, ADC_WIDTH_BIT_12, &raw) != ESP_OK) {
        return 0;
    }
    return (uint16_t)raw;
#endif
}

static void btn_adc_init(void) {
#ifdef ARDUINO_ARCH_ESP32
    analogReadResolution(12);
#else
    adc2_config_channel_atten(BTN_ADC_CHANNEL, ADC_ATTEN_DB_11);
#endif
}

/* Map a raw reading to a key using the windows, with hysteresis. */
static int btn_classify(uint32_t raw) {
    if (s_btn_state != BTN_NONE) {
        const key_window_t* w = &key_windows[s_btn_state];
        uint16_t lo = (w->min_raw > KEY_HYST_RAW) ? (uint16_t)(w->min_raw - KEY_HYST_RAW) : 0;
        uint16_t hi = (uint16_t)(w->max_raw + KEY_HYST_RAW);
        if (raw >= lo && raw <= hi) {
            return s_btn_state;
        }
    }
    for (int i = 0; i < BTN_COUNT; i++) {
        if (raw >= key_windows[i].min_raw && raw <= key_windows[i].max_raw) {
            return i;
        }
    }
    return BTN_NONE;
}

/* Returns the button on a fresh press edge, otherwise BTN_NONE. */
static int btn_poll_event(void) {
    int now = btn_classify(btn_read_raw());
    if (now == s_btn_candidate) {
        if (s_btn_candidate_count < BTN_STABLE_SAMPLES) {
            s_btn_candidate_count++;
        }
    } else {
        s_btn_candidate = now;
        s_btn_candidate_count = 1;
    }
    if (s_btn_candidate_count >= BTN_STABLE_SAMPLES && s_btn_candidate != s_btn_state) {
        int prev = s_btn_state;
        s_btn_state = s_btn_candidate;
        if (prev == BTN_NONE && s_btn_state != BTN_NONE) {
            return s_btn_state;
        }
    }
    return BTN_NONE;
}

/* ------------------------------------------------------------------------- */
/*  Drawing helpers                                                           */
/* ------------------------------------------------------------------------- */

static void checkError(enum EpdDrawError err) {
    if (err != EPD_DRAW_SUCCESS) {
        ESP_LOGE("ui", "draw error: %X", err);
    }
}

static int waveform_temperature(void) {
    int t = g_temperature;
    if (t < 20) {
        t = 20;
    } else if (t > 30) {
        t = 30;
    }
    return t;
}

static void full_refresh(void) {
    epd_power_enable();
    checkError(epd_hl_update_screen(&hl, MODE_GC16, waveform_temperature()));
    epd_power_disable();
}

/* Partial refresh of one logical rectangle.
 *
 * The installed epdiy build draws full-width native lines and skips unchanged
 * ones through hl.dirty_lines.  For INVERTED_PORTRAIT a rotated full-height
 * band {x,0,w,SH} maps exactly to the native lines covered by the element, so
 * only those lines are driven.  MODE_GL16 is the non-flashing grayscale mode. */
static void partial_refresh(EpdRect area) {
    memset(hl.dirty_lines, 0, (size_t)epd_height() * sizeof(bool));
    EpdRect band = {area.x, 0, area.width, SH};

    epd_power_enable();
    checkError(epd_hl_update_area(&hl, MODE_GL16, waveform_temperature(), band));
    epd_power_disable();
}

static EpdRect rect_union(EpdRect a, EpdRect b) {
    int x0 = a.x < b.x ? a.x : b.x;
    int y0 = a.y < b.y ? a.y : b.y;
    int x1 = (a.x + a.width) > (b.x + b.width) ? (a.x + a.width) : (b.x + b.width);
    int y1 = (a.y + a.height) > (b.y + b.height) ? (a.y + a.height) : (b.y + b.height);
    EpdRect r = {x0, y0, x1 - x0, y1 - y0};
    return r;
}

static int vcenter_baseline(const EpdFont* f, int y, int h) {
    return y + h / 2 + (f->ascender + f->descender) / 2;
}

static void put_text(
    const EpdFont* f, const char* s, int x, int baseline, uint8_t fg, uint8_t bg,
    uint32_t flags
) {
    EpdFontProperties p = epd_font_properties_default();
    p.fg_color = fg & 0x0F;
    p.bg_color = bg & 0x0F;
    p.flags = (enum EpdFontFlags)(EPD_DRAW_ALIGN_LEFT | flags);
    int cx = x, cy = baseline;
    epd_write_string(f, s, &cx, &cy, fb, &p);
}

static void put_center(
    const EpdFont* f, const char* s, int cx, int baseline, uint8_t fg, uint8_t bg,
    uint32_t flags
) {
    EpdFontProperties p = epd_font_properties_default();
    p.fg_color = fg & 0x0F;
    p.bg_color = bg & 0x0F;
    p.flags = (enum EpdFontFlags)(EPD_DRAW_ALIGN_CENTER | flags);
    int x = cx, y = baseline;
    epd_write_string(f, s, &x, &y, fb, &p);
}

static void put_right(
    const EpdFont* f, const char* s, int right_x, int baseline, uint8_t fg, uint8_t bg,
    uint32_t flags
) {
    EpdFontProperties p = epd_font_properties_default();
    p.fg_color = fg & 0x0F;
    p.bg_color = bg & 0x0F;
    p.flags = (enum EpdFontFlags)(EPD_DRAW_ALIGN_RIGHT | flags);
    int x = right_x, y = baseline;
    epd_write_string(f, s, &x, &y, fb, &p);
}

/* ------------------------------------------------------------------------- */
/*  Home screen layout                                                        */
/* ------------------------------------------------------------------------- */

static int y_status, y_hero, y_cards, y_grid, y_nav;
static int card_h, card_w, card_gap;
static int grid_x, grid_w, cell_w, cell_h, cell_gap;

static void layout_compute(void) {
    y_status = 0;
    y_hero = 76;
    y_cards = 570;
    card_h = 300;
    card_gap = 24;
    card_w = (SW - 3 * 24) / 2;
    y_grid = 910;
    y_nav = SH - 64;

    grid_x = 24;
    grid_w = SW - 2 * 24;
    cell_gap = 20;
    cell_w = (grid_w - 3 * cell_gap) / 4;
    cell_h = 120;
}

/* Ink-wash landscape background, pre-rotated to the native framebuffer
 * orientation and copied with the fast epd_copy_to_framebuffer path. */
static void draw_background(void) {
    epd_copy_to_framebuffer((EpdRect){0, 0, epd_width(), epd_height()}, img_bg_data, fb);
}

static void draw_statusbar(void) {
    epd_fill_rect((EpdRect){0, y_status, SW, 56}, PX(G_LGRAY), fb);
    put_text(font_body, "06:30", 24, vcenter_baseline(font_body, y_status, 56), G_BLACK, G_LGRAY, 0);
    put_right(font_body, "85%", SW - 24, vcenter_baseline(font_body, y_status, 56), G_BLACK, G_LGRAY, 0);
}

static void draw_hero(void) {
    int x = 80;
    put_text(font_clock, "06:30", x, y_hero + 154, G_BLACK, G_WHITE, 0);
    put_text(font_body, "5月20日 星期二", x, y_hero + 224, G_BLACK, G_WHITE, 0);
    put_text(font_body, "乙巳年四月廿三", x, y_hero + 274, G_BLACK, G_WHITE, 0);
    epd_draw_hline(x, y_hero + 314, 120, PX(G_DGRAY), fb);
    put_text(font_body, "慢下来，", x, y_hero + 370, G_BLACK, G_WHITE, 0);
    put_text(font_body, "让灵感跟上生活的温度。", x, y_hero + 420, G_BLACK, G_WHITE, 0);
}

static void draw_card_reading(EpdRect r) {
    epd_fill_rect(r, PX(G_LGRAY), fb);
    epd_draw_rect(r, PX(G_DGRAY), fb);
    int pad = 20;

    put_text(font_body, "正在阅读", r.x + pad, r.y + 46, G_BLACK, G_LGRAY, 0);
    put_text(font_body, "人间草木", r.x + pad, r.y + 120, G_BLACK, G_LGRAY, 0);
    put_text(font_body, "汪曾祺", r.x + pad, r.y + 160, G_BLACK, G_LGRAY, 0);

    /* progress bar: white background, black border, black fill at 45% */
    int bar_x = r.x + pad, bar_w = r.width - 2 * pad, bar_y = r.y + 194;
    epd_fill_rect((EpdRect){bar_x, bar_y, bar_w, 18}, PX(G_WHITE), fb);
    epd_draw_rect((EpdRect){bar_x, bar_y, bar_w, 18}, PX(G_BLACK), fb);
    epd_fill_rect((EpdRect){bar_x + 3, bar_y + 3, (bar_w - 6) * 45 / 100, 12}, PX(G_BLACK), fb);
    put_text(font_body, "阅读进度 45%", r.x + pad, r.y + 244, G_BLACK, G_LGRAY, 0);

    EpdRect btn = {r.x + pad, r.y + r.height - 56, 180, 40};
    epd_fill_rect(btn, PX(G_BLACK), fb);
    put_center(font_body, "继续阅读", btn.x + btn.width / 2, vcenter_baseline(font_body, btn.y, btn.height), G_WHITE, G_BLACK, 0);
}

static void draw_card_todo(EpdRect r) {
    epd_fill_rect(r, PX(G_LGRAY), fb);
    epd_draw_rect(r, PX(G_DGRAY), fb);
    int pad = 20;

    put_text(font_body, "今日待办", r.x + pad, r.y + 46, G_BLACK, G_LGRAY, 0);
    put_right(font_body, "+", r.x + r.width - pad, r.y + 46, G_BLACK, G_LGRAY, 0);

    const char* items[4] = {"读书30分钟", "整理设计方案", "练习钢琴", "早起"};
    const bool done[4] = {false, false, false, true};
    for (int i = 0; i < 4; i++) {
        int iy = r.y + 92 + i * 48;
        EpdRect box = {r.x + pad, iy - 12, 20, 20};
        epd_draw_rect(box, PX(G_BLACK), fb);
        if (done[i]) {
            epd_fill_rect(box, PX(G_BLACK), fb);
        }
        put_text(font_body, items[i], r.x + pad + 32, iy + 6, G_BLACK, G_LGRAY, 0);
    }
}

static const char* app_names[8] = {
    "书架", "笔记", "日历", "文件管理", "应用市场", "浏览器", "设置", "时钟",
};

static void draw_grid(void) {
    for (int row = 0; row < 2; row++) {
        for (int col = 0; col < 4; col++) {
            int idx = row * 4 + col;
            EpdRect cell = {
                grid_x + col * (cell_w + cell_gap), y_grid + row * (cell_h + 14), cell_w, cell_h};
            epd_fill_rect(cell, PX(G_WHITE), fb);
            epd_draw_rect(cell, PX(G_DGRAY), fb);
            EpdRect icon = {cell.x + cell.width / 4, cell.y + 8, cell.width / 2, 40};
            epd_draw_rect(icon, PX(G_DGRAY), fb);
            put_center(
                font_body, app_names[idx], cell.x + cell.width / 2, cell.y + cell.height - 14,
                G_BLACK, G_WHITE, 0
            );
        }
    }
}

static const char* nav_names[4] = {"首页", "发现", "灵感", "我的"};

static void draw_navbar(void) {
    epd_fill_rect((EpdRect){0, y_nav, SW, SH - y_nav}, PX(G_LGRAY), fb);
    int tab_w = SW / 4;
    for (int i = 0; i < 4; i++) {
        int cx = tab_w * i + tab_w / 2;
        put_center(font_body, nav_names[i], cx, vcenter_baseline(font_body, y_nav, SH - y_nav), G_BLACK, G_LGRAY, 0);
    }
}

/* ------------------------------------------------------------------------- */
/*  Reading screen                                                            */
/* ------------------------------------------------------------------------- */

enum { SCR_HOME = 0, SCR_READING };

static int s_screen = SCR_HOME;
static int s_read_page = 0;

/* Pre-wrapped reading text.  If you change this text, add the new characters
 * to .tools/gen_fonts.py and regenerate ui_sc_20.h. */
static const char* reading_lines[] = {
    "读书是用生活所感去读书，",
    "用读书所得去生活。",
    "",
    "在安静的午后，翻开一本书，",
    "让时间慢下来。",
    "",
    "窗外的风，杯中的茶，纸上的字，",
    "都是此刻的陪伴。",
    "",
    "愿每一次阅读，",
    "都能让心有片刻的安宁。",
    "",
    "愿你在喧嚣的世界里，",
    "仍能听见内心的声音。",
    "",
    "日子慢一点，心就静一点。",
    "一本书，一杯茶，一个下午，",
    "便是人间好时节。",
};
#define READING_LINE_COUNT ((int)(sizeof(reading_lines) / sizeof(reading_lines[0])))
#define READING_LINES_PER_PAGE 16
#define READING_PAGE_COUNT ((READING_LINE_COUNT + READING_LINES_PER_PAGE - 1) / READING_LINES_PER_PAGE)

static void draw_reading(void) {
    epd_hl_set_all_white(&hl);
    epd_fill_rect((EpdRect){0, 0, SW, SH}, PX(G_WHITE), fb);

    /* header */
    epd_fill_rect((EpdRect){0, 0, SW, 72}, PX(G_LGRAY), fb);
    put_text(
        font_body, "人间草木 · 汪曾祺", 32, vcenter_baseline(font_body, 0, 72), G_BLACK, G_LGRAY,
        0
    );
    char page[16];
    snprintf(page, sizeof(page), "%d / %d", s_read_page + 1, READING_PAGE_COUNT);
    put_right(
        font_body, page, SW - 32, vcenter_baseline(font_body, 0, 72), G_BLACK, G_LGRAY, 0
    );

    /* body */
    int y = 140;
    int first = s_read_page * READING_LINES_PER_PAGE;
    for (int i = 0; i < READING_LINES_PER_PAGE && first + i < READING_LINE_COUNT; i++) {
        put_text(font_body, reading_lines[first + i], 48, y, G_BLACK, G_WHITE, 0);
        y += 72;
    }

    /* footer hint */
    epd_fill_rect((EpdRect){0, SH - 56, SW, 56}, PX(G_LGRAY), fb);
    put_center(
        font_body, "UP / DOWN 翻页     OK 返回", SW / 2,
        vcenter_baseline(font_body, SH - 56, 56), G_BLACK, G_LGRAY, 0
    );
}

/* ------------------------------------------------------------------------- */
/*  Focus / navigation                                                        */
/* ------------------------------------------------------------------------- */

enum {
    FOC_READING = 0,
    FOC_TODO,
    FOC_APP0,
    FOC_NAV0,
};
#define FOC_APP(i) (FOC_APP0 + (i))
#define FOC_NAV(i) (FOC_NAV0 + (i))
#define FOC_APP_COUNT 8
#define FOC_NAV_COUNT 4
#define FOC_COUNT (2 + FOC_APP_COUNT + FOC_NAV_COUNT)

static int s_focus = 0;

static EpdRect focus_rect(int idx) {
    if (idx == FOC_READING) {
        return (EpdRect){24, y_cards, card_w, card_h};
    }
    if (idx == FOC_TODO) {
        return (EpdRect){24 + card_w + card_gap, y_cards, card_w, card_h};
    }
    if (idx >= FOC_APP0 && idx < FOC_APP0 + FOC_APP_COUNT) {
        int i = idx - FOC_APP0;
        int row = i / 4, col = i % 4;
        return (EpdRect){grid_x + col * (cell_w + cell_gap), y_grid + row * (cell_h + 14), cell_w, cell_h};
    }
    if (idx >= FOC_NAV0 && idx < FOC_NAV0 + FOC_NAV_COUNT) {
        int i = idx - FOC_NAV0;
        int tab_w = SW / 4;
        return (EpdRect){tab_w * i, y_nav, tab_w, SH - y_nav};
    }
    return (EpdRect){0, 0, 0, 0};
}

/* Draw the static home content (background + everything except the focus
 * highlight) into g_static_fb.  Called once at boot. */
static void draw_static(void) {
    uint8_t* save = fb;
    fb = g_static_fb;

    draw_background();
    draw_statusbar();
    draw_hero();
    draw_card_reading((EpdRect){24, y_cards, card_w, card_h});
    draw_card_todo((EpdRect){24 + card_w + card_gap, y_cards, card_w, card_h});
    draw_grid();
    draw_navbar();

    fb = save;
}

/* Copy the static content into the front framebuffer, then draw the focus
 * highlight on top. */
static void prepare_home(void) {
    memcpy(fb, g_static_fb, (size_t)SW * SH / 2);

    EpdRect r = focus_rect(s_focus);
    if (r.width > 0) {
        epd_draw_rect((EpdRect){r.x - 3, r.y - 3, r.width + 6, r.height + 6}, PX(G_WHITE), fb);
        epd_draw_rect((EpdRect){r.x - 1, r.y - 1, r.width + 2, r.height + 2}, PX(G_BLACK), fb);
    }
}

static void home_activate(void) {
    ESP_LOGI("ui", "activated focus %d", s_focus);
    if (s_focus == FOC_READING) {
        s_screen = SCR_READING;
        s_read_page = 0;
        draw_reading();
        full_refresh();
    } else {
        prepare_home();
        full_refresh();
    }
}

static void handle_event(int ev) {
    if (s_screen == SCR_READING) {
        if (ev == BTN_OK) {
            s_screen = SCR_HOME;
            prepare_home();
            full_refresh();
        } else if (ev == BTN_UP) {
            if (s_read_page > 0) {
                s_read_page--;
                draw_reading();
                partial_refresh((EpdRect){0, 0, SW, SH});
            }
        } else if (ev == BTN_DOWN) {
            if (s_read_page < READING_PAGE_COUNT - 1) {
                s_read_page++;
                draw_reading();
                partial_refresh((EpdRect){0, 0, SW, SH});
            }
        }
        return;
    }

    if (ev == BTN_UP || ev == BTN_DOWN) {
        int old = s_focus;
        s_focus = (s_focus + ((ev == BTN_DOWN) ? 1 : FOC_COUNT - 1)) % FOC_COUNT;
        prepare_home();
        partial_refresh(rect_union(focus_rect(old), focus_rect(s_focus)));
    } else if (ev == BTN_OK) {
        home_activate();
    }
}

/* ------------------------------------------------------------------------- */
/*  Setup / main loop                                                         */
/* ------------------------------------------------------------------------- */

void idf_setup() {
    gpio_set_direction(EPD_PWR_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(EPD_PWR_PIN, 0);

    epd_init(&DEMO_BOARD, &ED060KD1, EPD_LUT_64K);
    epd_set_vcom(1560);
    hl = epd_hl_init(WAVEFORM);
    epd_set_rotation(EPD_ROT_INVERTED_PORTRAIT);

    SW = epd_rotated_display_width();
    SH = epd_rotated_display_height();
    layout_compute();

    fb = epd_hl_get_framebuffer(&hl);
    g_temperature = (int)epd_ambient_temperature();

    printf("Dimensions after rotation, width: %d height: %d\n\n", SW, SH);

    btn_adc_init();
    ESP_LOGI("ui", "button ADC on GPIO%d, current raw = %u", BTN_ADC_PIN, (unsigned)btn_read_raw());

    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);

    /* clear the panel once at boot */
    epd_power_enable();
    epd_clear();
    epd_power_disable();

    /* Render the static content once into a PSRAM copy so focus moves only
     * need a memcpy + partial refresh instead of redrawing the background. */
    g_static_fb = heap_caps_malloc((size_t)SW * SH / 2, MALLOC_CAP_SPIRAM);
    assert(g_static_fb != NULL);
    draw_static();

    prepare_home();
    full_refresh();
}

#ifndef ARDUINO_ARCH_ESP32
void delay(uint32_t millis) { vTaskDelay(millis / portTICK_PERIOD_MS); }
#endif

void idf_loop() {
    for (;;) {
        int ev = btn_poll_event();
        if (ev != BTN_NONE) {
            ESP_LOGI("ui", "button event: %d", ev);
            handle_event(ev);
        }
        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    }
}

#ifndef ARDUINO_ARCH_ESP32
void app_main() {
    idf_setup();
    while (1) {
        idf_loop();
    };
}
#endif
