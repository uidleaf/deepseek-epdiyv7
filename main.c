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
#include <math.h>

#include <driver/gpio.h>

#include <epdiy.h>

#include "sdkconfig.h"

/* Subset Chinese fonts (generated with scripts/fontconvert.py). */
#include "ui_sc_20.h"
#include "ui_clock.h"
#include "img_bg.h"
#include "app_icons.h"

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

/* Direct update (MODE_DU): purely draws black/white pixels directly without
 * any intermediate black or white clearing pulses. Zero flash, instant draw. */
static void direct_refresh(EpdRect area) {
    memset(hl.dirty_lines, 0, (size_t)epd_height() * sizeof(bool));
    EpdRect band = {area.x, 0, area.width, SH};

    epd_power_enable();
    checkError(epd_hl_update_area(&hl, MODE_DU, waveform_temperature(), band));
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

static EpdRect focus_union(EpdRect a, EpdRect b) {
    EpdRect u = rect_union(a, b);
    int x = (u.x >= 4) ? u.x - 4 : 0;
    int y = (u.y >= 4) ? u.y - 4 : 0;
    int w = u.width + 8;
    int h = u.height + 8;
    if (x + w > SW) w = SW - x;
    if (y + h > SH) h = SH - y;
    return (EpdRect){x, y, w, h};
}

static void draw_rounded_rect(EpdRect r, int rad, uint8_t color, int thickness) {
    if (r.width <= 0 || r.height <= 0) return;
    for (int t = 0; t < thickness; t++) {
        epd_draw_hline(r.x + rad, r.y + t, r.width - 2 * rad, color, fb);
        epd_draw_hline(r.x + rad, r.y + r.height - 1 - t, r.width - 2 * rad, color, fb);
        epd_draw_vline(r.x + t, r.y + rad, r.height - 2 * rad, color, fb);
        epd_draw_vline(r.x + r.width - 1 - t, r.y + rad, r.height - 2 * rad, color, fb);
    }
    for (int dy = 0; dy <= rad; dy++) {
        for (int dx = 0; dx <= rad; dx++) {
            int d2 = (rad - dx) * (rad - dx) + (rad - dy) * (rad - dy);
            int r_outer2 = rad * rad;
            int r_inner2 = (rad - thickness) * (rad - thickness);
            if (d2 <= r_outer2 && d2 >= r_inner2) {
                epd_draw_pixel(r.x + dx, r.y + dy, color, fb);
                epd_draw_pixel(r.x + r.width - 1 - dx, r.y + dy, color, fb);
                epd_draw_pixel(r.x + dx, r.y + r.height - 1 - dy, color, fb);
                epd_draw_pixel(r.x + r.width - 1 - dx, r.y + r.height - 1 - dy, color, fb);
            }
        }
    }
}

static void fill_rounded_rect(EpdRect r, int rad, uint8_t color) {
    if (r.width <= 0 || r.height <= 0) return;
    epd_fill_rect((EpdRect){r.x, r.y + rad, r.width, r.height - 2 * rad}, color, fb);
    for (int dy = 0; dy < rad; dy++) {
        int dx = rad - (int)sqrtf((float)(rad * rad - (rad - dy) * (rad - dy)));
        epd_draw_hline(r.x + dx, r.y + dy, r.width - 2 * dx, color, fb);
        epd_draw_hline(r.x + dx, r.y + r.height - 1 - dy, r.width - 2 * dx, color, fb);
    }
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
static int card_h, card_w, card1_x, card2_x, nav_h;

static const int col_centers[4] = {136, 403, 670, 936};
static const int row_ys[2] = {864, 1060};

static void layout_compute(void) {
    y_status = 0;
    y_hero = 76;
    y_cards = 520;
    card_h = 300;
    card_w = 480;
    card1_x = 36;
    card2_x = SW - 36 - card_w;
    y_grid = 864;
    y_nav = 1315;
    nav_h = 100;
}

/* Ink-wash landscape background, pre-rotated to the native framebuffer
 * orientation and copied with the fast epd_copy_to_framebuffer path. */
static void draw_background(void) {
    epd_copy_to_framebuffer((EpdRect){0, 0, epd_width(), epd_height()}, img_bg_data, fb);
}

static void draw_statusbar(void) {
    epd_fill_rect((EpdRect){0, y_status, SW, 56}, PX(G_WHITE), fb);
    put_text(font_body, "06:30", 32, vcenter_baseline(font_body, y_status, 56), G_BLACK, G_WHITE, 0);
    put_right(font_body, "85%", SW - 32, vcenter_baseline(font_body, y_status, 56), G_BLACK, G_WHITE, 0);
}

static void draw_hero(void) {
    int x = 80;
    put_text(font_clock, "06:30", x, y_hero + 130, G_BLACK, G_WHITE, 0);
    put_text(font_body, "5月20日 星期二", x, y_hero + 195, G_BLACK, G_WHITE, 0);
    put_text(font_body, "乙巳年四月廿三", x, y_hero + 240, G_BLACK, G_WHITE, 0);
    epd_fill_rect((EpdRect){x, y_hero + 265, 120, 3}, PX(G_BLACK), fb);
    put_text(font_body, "慢下来，", x, y_hero + 310, G_BLACK, G_WHITE, 0);
    put_text(font_body, "让灵感跟上生活的温度。", x, y_hero + 355, G_BLACK, G_WHITE, 0);
}

static void draw_card_reading(EpdRect r) {
    fill_rounded_rect(r, 24, PX(G_WHITE));
    draw_rounded_rect(r, 24, PX(G_BLACK), 3);
    int pad = 28;

    put_text(font_body, "正在阅读", r.x + pad, r.y + 44, G_BLACK, G_WHITE, 0);
    put_text(font_body, "人间草木", r.x + pad, r.y + 98, G_BLACK, G_WHITE, 0);
    put_text(font_body, "汪曾祺", r.x + pad, r.y + 140, G_BLACK, G_WHITE, 0);

    /* progress line */
    int y_prog = r.y + 175;
    int prog_w = 200;
    epd_fill_rect((EpdRect){r.x + pad, y_prog, prog_w, 4}, PX(G_LGRAY), fb);
    epd_fill_rect((EpdRect){r.x + pad, y_prog, prog_w * 45 / 100, 4}, PX(G_BLACK), fb);
    put_text(font_body, "阅读进度 45%", r.x + pad, y_prog + 32, G_BLACK, G_WHITE, 0);

    /* capsule button: unselected default in static buffer */
    EpdRect btn = {r.x + pad, r.y + r.height - 60, BTN_READING_W, BTN_READING_H};
    epd_draw_rotated_image(btn, btn_reading_unsel_data, fb);

    /* book cover thumbnail on the right */
    EpdRect book = {r.x + r.width - pad - BOOK_THUMB_W, r.y + 20, BOOK_THUMB_W, BOOK_THUMB_H};
    epd_draw_rotated_image(book, book_thumb_data, fb);
}

static void draw_card_todo(EpdRect r) {
    fill_rounded_rect(r, 24, PX(G_WHITE));
    draw_rounded_rect(r, 24, PX(G_BLACK), 3);
    int pad = 28;

    put_text(font_body, "今日待办", r.x + pad, r.y + 44, G_BLACK, G_WHITE, 0);

    /* circled plus icon */
    int plus_cx = r.x + r.width - pad - 16;
    int plus_cy = r.y + 36;
    epd_draw_circle(plus_cx, plus_cy, 15, PX(G_BLACK), fb);
    epd_draw_circle(plus_cx, plus_cy, 14, PX(G_BLACK), fb);
    epd_draw_circle(plus_cx, plus_cy, 13, PX(G_BLACK), fb);
    epd_fill_rect((EpdRect){plus_cx - 8, plus_cy - 1, 17, 3}, PX(G_BLACK), fb);
    epd_fill_rect((EpdRect){plus_cx - 1, plus_cy - 8, 3, 17}, PX(G_BLACK), fb);

    const char* tasks[4] = {"读书30分钟", "整理设计方案", "练习钢琴", "早起"};
    for (int i = 0; i < 4; i++) {
        int iy = r.y + 92 + i * 50;
        int cx = r.x + pad + 14;
        int cy = iy + 14;
        if (i < 3) {
            epd_draw_circle(cx, cy, 11, PX(G_BLACK), fb);
            epd_draw_circle(cx, cy, 10, PX(G_BLACK), fb);
            epd_draw_circle(cx, cy, 9, PX(G_BLACK), fb);
        } else {
            epd_fill_circle(cx, cy, 11, PX(G_BLACK), fb);
            for (int t = -1; t <= 1; t++) {
                epd_draw_line(cx - 5, cy + t, cx - 1, cy + 4 + t, PX(G_WHITE), fb);
                epd_draw_line(cx - 1, cy + 4 + t, cx + 5, cy - 3 + t, PX(G_WHITE), fb);
            }
        }
        put_text(font_body, tasks[i], r.x + pad + 40, iy + 24, G_BLACK, G_WHITE, 0);
    }
}

static const char* app_names[8] = {
    "书架", "笔记", "日历", "文件管理", "应用市场", "浏览器", "设置", "时钟",
};

static void draw_grid(void) {
    for (int r = 0; r < 2; r++) {
        for (int c = 0; c < 4; c++) {
            int idx = r * 4 + c;
            int cx = col_centers[c];
            int cy = row_ys[r];
            EpdRect icon_rect = {cx - ICON_W / 2, cy, ICON_W, ICON_H};
            epd_draw_rotated_image(icon_rect, g_icons_unselected[idx], fb);
            put_center(font_body, app_names[idx], cx, cy + ICON_H + 30, G_BLACK, G_WHITE, 0);
        }
    }
}

static void draw_pagination(void) {
    int y_dots = 1250;
    epd_fill_circle(SW / 2 - 22, y_dots, 6, PX(G_BLACK), fb);
    epd_draw_circle(SW / 2, y_dots, 6, PX(G_BLACK), fb);
    epd_draw_circle(SW / 2, y_dots, 5, PX(G_BLACK), fb);
    epd_draw_circle(SW / 2 + 22, y_dots, 6, PX(G_BLACK), fb);
    epd_draw_circle(SW / 2 + 22, y_dots, 5, PX(G_BLACK), fb);
}

static const char* nav_names[4] = {"首页", "发现", "灵感", "我的"};

static void draw_navbar(void) {
    int nav_w = SW - card1_x * 2;
    EpdRect nav_rect = {card1_x, y_nav, nav_w, nav_h};
    fill_rounded_rect(nav_rect, nav_h / 2, PX(G_WHITE));
    draw_rounded_rect(nav_rect, nav_h / 2, PX(G_BLACK), 3);

    for (int i = 0; i < 4; i++) {
        int cx = col_centers[i];
        EpdRect ir = {cx - NAV_ICON_W / 2, y_nav + 14, NAV_ICON_W, NAV_ICON_H};
        epd_draw_rotated_image(ir, g_nav_icons[i], fb);
        put_center(font_body, nav_names[i], cx, y_nav + 78, G_BLACK, G_WHITE, 0);
    }
}

/* ------------------------------------------------------------------------- */
/*  Reading screen                                                            */
/* ------------------------------------------------------------------------- */

enum { SCR_HOME = 0, SCR_READING };

static int s_screen = SCR_HOME;
static int s_read_page = 0;

/* Pre-wrapped reading text for 6-inch ED060KD1 (1072x1448).
 * 6 full continuous pages from Wang Zengqi"s "人间草木".
 * 21 characters per line, 2-char indent, standard CJK wrapping rules. */
static const char* reading_lines[] = {
    /* --- Page 1 --- */
    "　　如果你来访我，我不在，请和我门外的花坐",
    "一会儿。它们很温暖，我注视它们很多很多日子",
    "了。它们不知道我的名字，但我认得它们每一朵",
    "盛开的模样。它们在晨光中苏醒，在微风里舒展",
    "着柔嫩的花瓣，静静记录着光阴的流转。",
    "　　一定要爱着点什么，恰似草木对光阴的钟",
    "情。人总要呆在一种什么东西里，沉溺其中。苟",
    "有所得，才能证实自己的存在，切实地活出滋味",
    "来。我以为，最美的日子，不过是草木知秋，见",
    "微知著。在平凡琐碎的人间烟火中，寻觅属于自",
    "己的一分清欢。",
    "　　慢下来，听微风穿过树叶的轻响，看午后温",
    "暖的阳光落在泛黄纸页上的温度。草木有情，人",
    "间有味。",
    /* --- Page 2 --- */
    "　　昆明的雨季是明亮的、丰满的，也是使人动",
    "情的。城里城外，到处是绿的。草木的枝叶水分",
    "都足足的，新发的嫩芽油光发亮。雨季的花极",
    "多，缅桂花、木槿花，开得满树皆是。",
    "　　栀子花粗粗大大，又香得掸都掸不开，于是",
    "为文雅人不取，以为品格不高。栀子花说：“去",
    "你的，我就是要这样香，香得痛痛快快，你们管",
    "得着吗！”这种泼辣劲儿，让人打心眼里痛快喜",
    "欢。",
    "　　雨季里菌子也极多，牛肝菌、青头菌、干巴",
    "菌，味道鲜美绝伦。人在雨中走着，不觉得冷，",
    "只觉得浑身湿润清爽。四方草木，各得其所，人",
    "间至味，最是清欢。",
    "",
    /* --- Page 3 --- */
    "　　初春，微风一吹，荠菜刚从枯草丛中冒头，",
    "绿意盈盈。北京人说这是“吃春”，咬一口春天",
    "的鲜嫩。柳芽初吐时，采嫩柳芽，沸水焯过，凉",
    "拌，微苦清香，最解春困。几箸下肚，口舌生",
    "津。",
    "　　春雨淅淅沥沥，枸杞初生嫩苗，摘其嫩头与",
    "极嫩的豆腐同拌，清香扑鼻。竹林里春笋破土而",
    "出，带着泥土的清甜与芬芳。春天的韭菜也是极",
    "好的，头刀韭菜炒鸡蛋，嫩绿金黄，鲜美无匹。",
    "　　正如东坡所云：蒌蒿满地芦芽短，正是河豚",
    "欲上时。苏东坡真是个极会生活的可爱之人。一",
    "草一木，一粥一饭，生活是很好玩的，只要你用",
    "心去尝、去品、去感受。寻常巷陌，皆有诗意。",
    "",
    /* --- Page 4 --- */
    "　　夏天是属于荷花与西瓜的。荷塘里荷叶田",
    "田，如碧玉铺就，连绵无际。阵雨过后，荷叶上",
    "水珠滚来滚去，晶莹剔透，可爱极了。红荷初",
    "绽，亭亭玉立，清香远溢，沁人心脾。",
    "　　小院里搭起葡萄架，青绿的葡萄一串串挂在",
    "茂密的绿叶之间。夏夜，搬一把竹椅坐在葡萄架",
    "下，摇着蒲扇，听草丛里的虫鸣与远处的蛙声。",
    "天热极时，切开冰镇西瓜，咬上一大口，暑气全",
    "消。",
    "　　晚风清凉，星河璀璨，时光就这么悠悠地走",
    "着。看萤火虫在草叶间提灯夜行，忽明忽暗。平",
    "淡之中满是踏实与从容，岁月静好，莫过于此。",
    "",
    "",
    /* --- Page 5 --- */
    "　　到了八月，桂花开了。那香气是不可阻挡",
    "的，顺着清凉的秋风，飘满了整座小城的深巷。",
    "清晨走在石阶上，捡几朵落在青苔上的碎金，夹",
    "在书页里，整个秋天就都有了温润的香气。",
    "　　秋海棠静静开在阶前，红艳娇嫩。看红叶在",
    "枝头慢慢变深，看银杏叶像一把把金色小扇落满",
    "石板小径。院子里的柿子树挂满了红彤彤的小灯",
    "笼，在清冷的夜风中微微摇曳，透着丰足的喜",
    "气。",
    "　　秋天是丰收的季节，也是沉淀的季节。洗尽",
    "铅华，草木归真，天地间一片澄明辽阔。心中若",
    "有桃花源，何处不是水云间。捧一杯热茶，静听",
    "落叶萧萧，心中自在安宁。",
    "",
    /* --- Page 6 --- */
    "　　隆冬时节，大雪纷飞，天地一白。案头清",
    "供，不过是水仙一丛，腊梅数枝，天竹果几颗。",
    "红白相间，在素白的天地里分外幽香。哪怕室外",
    "冰天雪地，屋里有一盆花，便有了春意与生机。",
    "　　绿蚁新醅酒，红泥小火炉。晚来天欲雪，能",
    "饮一杯无。屋外寒风呼啸，屋内暖意融融。翻开",
    "一卷泛黄的旧书，字里行间皆是岁月留下的温润",
    "痕迹。除夕守岁，最是家人团圆暖人心。",
    "　　草木荣枯，四时更替。看窗外白雪覆阶，炉",
    "上水汽氤氲。愿你心中常驻一片青绿，不疾不",
    "徐，温柔且坚定地走向每一个明朗的清晨。岁岁",
    "常欢愉，万事皆顺遂。",
    "",
    "",
};
#define READING_LINE_COUNT ((int)(sizeof(reading_lines) / sizeof(reading_lines[0])))
#define READING_LINES_PER_PAGE 14
#define READING_PAGE_COUNT ((READING_LINE_COUNT + READING_LINES_PER_PAGE - 1) / READING_LINES_PER_PAGE)

static void draw_reading(void) {
    epd_fill_rect((EpdRect){0, 0, SW, SH}, PX(G_WHITE), fb);

    /* header: white background + black text + subtle divider */
    put_text(
        font_body, "人间草木 · 汪曾祺", 95, vcenter_baseline(font_body, 0, 76), G_BLACK, G_WHITE,
        0
    );
    char page[32];
    snprintf(page, sizeof(page), "第 %d / %d 页", s_read_page + 1, READING_PAGE_COUNT);
    put_right(
        font_body, page, SW - 95, vcenter_baseline(font_body, 0, 76), G_BLACK, G_WHITE, 0
    );
    epd_draw_hline(95, 84, SW - 190, PX(G_BLACK), fb);

    /* body */
    int y = 140;
    int first = s_read_page * READING_LINES_PER_PAGE;
    for (int i = 0; i < READING_LINES_PER_PAGE && first + i < READING_LINE_COUNT; i++) {
        if (reading_lines[first + i][0] != '\0') {
            put_text(font_body, reading_lines[first + i], 95, y, G_BLACK, G_WHITE, 0);
        }
        y += 80;
    }

    /* footer hint: white background + top divider + centered text */
    epd_draw_hline(95, SH - 90, SW - 190, PX(G_BLACK), fb);
    put_center(
        font_body, "UP 上一页   ·   DOWN 下一页   ·   OK 返回主页", SW / 2,
        vcenter_baseline(font_body, SH - 90, 80), G_BLACK, G_WHITE, 0
    );
}

/* Redraw only the body text and page number for page turns.
 * The header title and footer divider/text are NOT modified at all,
 * keeping static elements 100% frozen without any flicker. */
static void draw_reading_body(void) {
    /* clear body area only (between header divider and footer divider) */
    epd_fill_rect((EpdRect){0, 85, SW, SH - 85 - 91}, PX(G_WHITE), fb);

    /* update page number on pure white background */
    epd_fill_rect((EpdRect){SW / 2, 0, SW / 2, 83}, PX(G_WHITE), fb);
    char page[32];
    snprintf(page, sizeof(page), "第 %d / %d 页", s_read_page + 1, READING_PAGE_COUNT);
    put_right(
        font_body, page, SW - 95, vcenter_baseline(font_body, 0, 76), G_BLACK, G_WHITE, 0
    );

    /* redraw body text */
    int y = 140;
    int first = s_read_page * READING_LINES_PER_PAGE;
    for (int i = 0; i < READING_LINES_PER_PAGE && first + i < READING_LINE_COUNT; i++) {
        if (reading_lines[first + i][0] != '\0') {
            put_text(font_body, reading_lines[first + i], 95, y, G_BLACK, G_WHITE, 0);
        }
        y += 80;
    }
}

/* ------------------------------------------------------------------------- */
/*  Focus / navigation                                                        */
/* ------------------------------------------------------------------------- */

enum {
    FOC_READING = 0,
    FOC_APP0,
    FOC_NAV0 = FOC_APP0 + 8,
};
#define FOC_APP(i) (FOC_APP0 + (i))
#define FOC_NAV(i) (FOC_NAV0 + (i))
#define FOC_APP_COUNT 8
#define FOC_NAV_COUNT 4
#define FOC_COUNT (1 + FOC_APP_COUNT + FOC_NAV_COUNT)

static int s_focus = 0;

static EpdRect focus_rect(int idx) {
    if (idx == FOC_READING) {
        int pad = 28;
        return (EpdRect){card1_x + pad, y_cards + card_h - 60, BTN_READING_W, BTN_READING_H};
    }
    if (idx >= FOC_APP0 && idx < FOC_APP0 + FOC_APP_COUNT) {
        int i = idx - FOC_APP0;
        int r = i / 4, c = i % 4;
        return (EpdRect){col_centers[c] - ICON_W / 2, row_ys[r], ICON_W, ICON_H};
    }
    if (idx >= FOC_NAV0 && idx < FOC_NAV0 + FOC_NAV_COUNT) {
        int i = idx - FOC_NAV0;
        int cx = col_centers[i];
        return (EpdRect){cx - 50, y_nav + 6, 100, nav_h - 12};
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
    draw_card_reading((EpdRect){card1_x, y_cards, card_w, card_h});
    draw_card_todo((EpdRect){card2_x, y_cards, card_w, card_h});
    draw_grid();
    draw_pagination();
    draw_navbar();

    fb = save;
}

static void draw_focus_box(EpdRect r) {
    if (r.width <= 0 || r.height <= 0) return;
    draw_rounded_rect(r, 16, PX(G_BLACK), 3);
}

/* Copy the static content into the front framebuffer, then draw the focus
 * highlight on top. */
static void prepare_home(void) {
    memcpy(fb, g_static_fb, (size_t)SW * SH / 2);

    if (s_focus == FOC_READING) {
        /* Highlight only the "继续阅读" button itself using crisp pre-rendered bitmap */
        EpdRect btn = focus_rect(FOC_READING);
        epd_draw_rotated_image(btn, btn_reading_sel_data, fb);
    } else if (s_focus >= FOC_APP0 && s_focus < FOC_APP0 + FOC_APP_COUNT) {
        /* Draw the bold selected state version of the icon */
        int idx = s_focus - FOC_APP0;
        EpdRect icon_r = focus_rect(s_focus);
        epd_draw_rotated_image(icon_r, g_icons_selected[idx], fb);
    } else if (s_focus >= FOC_NAV0 && s_focus < FOC_NAV0 + FOC_NAV_COUNT) {
        draw_focus_box(focus_rect(s_focus));
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
        direct_refresh(focus_union(focus_rect(s_focus), focus_rect(s_focus)));
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
                draw_reading_body();
                direct_refresh((EpdRect){0, 0, SW, SH});
            }
        } else if (ev == BTN_DOWN) {
            if (s_read_page < READING_PAGE_COUNT - 1) {
                s_read_page++;
                draw_reading_body();
                direct_refresh((EpdRect){0, 0, SW, SH});
            }
        }
        return;
    }

    if (ev == BTN_UP || ev == BTN_DOWN) {
        int old = s_focus;
        s_focus = (s_focus + ((ev == BTN_DOWN) ? 1 : FOC_COUNT - 1)) % FOC_COUNT;
        prepare_home();
        direct_refresh(focus_union(focus_rect(old), focus_rect(s_focus)));
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
