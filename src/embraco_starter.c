#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <gui/canvas.h>
#include <input/input.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <dialogs/dialogs.h>
#include <stdbool.h>
#include <stdio.h>

/*** PWM wiring (Flipper external header):
 *  + signal: PA7 (external pin "2 (A7)")
 *  - GND:    pin "8 (GND)")
***/
static const GpioPin* PWM_PIN = &gpio_ext_pa7;

/* ---------- Geometry / constants ---------- */
enum {
    CANVAS_W        = 128,
    CANVAS_H        = 64,

    TITLE_Y         = 14,       /* baseline for primary title */
    ROW_Y0          = 26,       /* first menu row baseline */
    ROW_DY          = 12,       /* rows step: 26,38,50,62 */

    SCROLLBAR_X     = 124,      /* dotted rail x */
    SCROLLBAR_W     = 3,
    SCROLLBAR_Y0    = 2,
    SCROLLBAR_Y1    = 62,

    TIMER_MARGIN    = 6,        /* gap from scrollbar to timer text */
};

/* ---------- Safe GPIO helpers ---------- */
static inline void pin_to_hiz(void) {
    /* Hi-Z (no pulls) — completely disconnected output */
    furi_hal_gpio_init(PWM_PIN, GpioModeInput, GpioPullNo, GpioSpeedLow);
}
static inline void pin_to_pp_low(void) {
    /* Safe push-pull LOW (actively pulls line low) */
    furi_hal_gpio_init(PWM_PIN, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);
    furi_hal_gpio_write(PWM_PIN, false);
}

/* ---------- Hardware PWM on PA7 ---------- */
#define PWM_CH FuriHalPwmOutputIdTim1PA7

static inline void pwm_hw_stop_safe(bool* running) {
    if(running && *running) {
        furi_hal_pwm_stop(PWM_CH);
        furi_delay_ms(1);
        *running = false;
    }
}
static inline void pwm_hw_start_safe(uint32_t freq_hz, bool* running) {
    /* 50% duty in hardware */
    furi_hal_pwm_start(PWM_CH, freq_hz, 50);
    if(running) *running = true;
}

/* ---------- Modes (powered menu) ---------- */
/* "Stand by" = PP LOW (no PWM). Low/Mid/Max use PWM.
 * "Power off" — отдельный пункт меню (не в этом массиве), он переводит систему в Hi‑Z и в «безопасное меню».
 */
typedef struct {
    const char* name;
    uint32_t    freq_hz;        /* 0 = no PWM (Stand by) */
    uint8_t     led_blink_hz;   /* 0/1/2/4 */
    uint32_t    default_secs;   /* timeout when Limit=Yes (0 => unlimited per-mode) */
} Mode;

static const Mode kModes[] = {
    {"Stand by", 0,   0,   0},   /* 0 — PP LOW, без таймера */
    {"Low speed", 55, 1, 120},   /* 1 — 2 min */
    {"Mid speed", 100,2,  60},   /* 2 — 1 min */
    {"Max speed", 160,4,  30},   /* 3 — 30 s */
};
#define MODE_COUNT (sizeof(kModes)/sizeof(kModes[0]))

/* ---------- Help text per inverter (no header) ---------- */
static const char* HELP_EMBRACO[] = {
    "Connect wires as follows:",
    "",
    "2 (A7)    -> inverter +",
    "(usually RED wire)",
    "8 (GND)  -> inverter -",
    "(usually WHITE wire)",
    "",
    "Note:",
    "This app provides",
    "3 test speeds:",
    "",
    "Low speed:",
    "2000 RPM (VNE)",
    "1800 RPM (VEG, FMF)",
    "",
    "Mid speed:",
    "3000 RPM",
    "(VNE, VEG, FMF)",
    "",
    "Max speed:",
    "4500 RPM",
    "(VNE, VEG, FMF)",
    "",
    "Embraco compressors",
    "support many speeds",
    "with 30 RPM steps.",
    "",
    "----------------",
    "",
    "App created by",
    "Adam Gray",
    "Founder of",
    "Expert Hub",
    "experthub.app",
    "",
    "----------------",
    "",
    "Press BACK to start.",
};
#define HELP_EMBRACO_COUNT (sizeof(HELP_EMBRACO)/sizeof(HELP_EMBRACO[0]))

static const char* HELP_SAMSUNG[] = {
    "In development",
};
#define HELP_SAMSUNG_COUNT (sizeof(HELP_SAMSUNG)/sizeof(HELP_SAMSUNG[0]))

/* ---------- State machine ---------- */
typedef enum {
    ScreenSelectInverter = 0,   /* первый экран: выбор инвертора */
    ScreenMenu,                 /* главное меню (динамическое) */
    ScreenHelp,
    ScreenSettings,
} ScreenId;

typedef enum {
    InvEmbraco = 0,
    InvSamsung = 1,
} InverterId;

/* ---------- App state ---------- */
typedef struct {
    /* where we are */
    ScreenId screen;

    /* inverter identity (affects title; later may affect mode presets) */
    InverterId inverter;

    /* powered flag:
     * false => «безопасное меню» (Power on / Settings / Help),
     * true  => «рабочее меню» (Stand by / Low / Mid / Max / Power off / Settings / Help).
     */
    bool powered;

    /* main menu navigation */
    uint8_t cursor;         /* visual row index */
    uint8_t first_visible;  /* top row in 4-line window */
    uint8_t active;         /* 0..MODE_COUNT-1 — selected powered mode (checkmark on right) */

    /* help scroll */
    uint8_t help_top_line;

    /* settings */
    bool limit_runtime;     /* Yes/No — per-mode timeout enforcement */
    bool arrow_captcha;     /* Yes/No — placeholder toggle (default Yes) */

    /* LED blink */
    NotificationApp* notif;
    FuriTimer* led_timer;
    bool led_on;

    /* PWM running flag */
    bool pwm_running;

    /* back-hint overlay */
    bool hint_visible;
    FuriTimer* hint_timer;

    /* countdown / auto-off */
    FuriTimer* tick_timer;  /* 1 Hz UI update */
    FuriTimer* off_timer;   /* one-shot precise auto-off */
    uint32_t remaining_ms;  /* 0 if none */
    bool timeout_expired;   /* event flag serviced in loop */

    /* IO */
    Gui* gui;
    ViewPort* vp;
    FuriMessageQueue* q;
} AppState;

/* ---------- LED helpers ---------- */
static void led_set(NotificationApp* n, bool on){
    if(!n) return;
    if(on) notification_message(n, &sequence_set_green_255);
    else   notification_message(n, &sequence_reset_rgb);
}
static void led_timer_cb(void* ctx){
    AppState* s = ctx;
    s->led_on = !s->led_on;
    led_set(s->notif, s->led_on);
}
static void led_apply(AppState* s, uint8_t blink_hz){
    if(s->led_timer){
        furi_timer_stop(s->led_timer);
        furi_timer_free(s->led_timer);
        s->led_timer = NULL;
    }
    s->led_on = false;
    led_set(s->notif, false);

    if(blink_hz == 0) return;
    uint32_t ms = 1000U / (blink_hz * 2U); /* toggle period for 50% blink */
    if(ms == 0) ms = 1;
    s->led_timer = furi_timer_alloc(led_timer_cb, FuriTimerTypePeriodic, s);
    furi_timer_start(s->led_timer, furi_ms_to_ticks(ms));
}

/* ---------- Dotted scrollbar (Momentum-like) ---------- */
static void draw_scrollbar_dotted(Canvas* c, uint16_t total_steps, uint16_t pos){
    if(total_steps <= 1) return;

    const uint16_t x  = SCROLLBAR_X;
    const uint16_t y0 = SCROLLBAR_Y0;
    const uint16_t y1 = SCROLLBAR_Y1;

    /* rail (dotted) */
    for(uint16_t y = y0; y <= y1; y += 3){
        canvas_draw_dot(c, x, y);
    }

    /* thumb position (instant to cursor) */
    const uint16_t track_h = (uint16_t)(y1 - y0);
    uint16_t denom = (total_steps > 1) ? (uint16_t)(total_steps - 1) : 1;
    uint16_t thumb_y = (uint16_t)(y0 + (pos * track_h) / denom);

    /* let the bottom of thumb "eat" last dot and rest on the screen bottom:
       thumb is 4px high, we center it around thumb_y (-> top at thumb_y-1) */
    if(thumb_y > (uint16_t)(y1 - 1)) thumb_y = (uint16_t)(y1 - 1);
    if(thumb_y < y0) thumb_y = y0;

    canvas_draw_box(c, (uint16_t)(x - 1), (uint16_t)(thumb_y - 1), SCROLLBAR_W, 4);
}

/* ---------- Pretty checkmark (7x7), lowered by 1px ---------- */
static void draw_checkmark(Canvas* c, int x, int baseline_y){
    /* draw as two joined 1px segments, visually balanced */
    int y = baseline_y - 6; /* подняли на 2 px выше */
    canvas_draw_line(c, x,     y+3, x+2, y+5);
    canvas_draw_line(c, x+2,   y+5, x+7, y   );
}

/* ---------- Countdown & auto-off ---------- */
static void tick_timer_cb(void* ctx){
    AppState* s = ctx;
    if(s->remaining_ms >= 1000) s->remaining_ms -= 1000;
    else s->remaining_ms = 0;
    if(s->vp) view_port_update(s->vp);
}
static void off_timer_cb(void* ctx){
    AppState* s = ctx;
    s->remaining_ms = 0;
    s->timeout_expired = true;
    if(s->vp) view_port_update(s->vp);
}
static void stop_timers(AppState* s){
    if(s->tick_timer) furi_timer_stop(s->tick_timer);
    if(s->off_timer)  furi_timer_stop(s->off_timer);
}
static void free_timers(AppState* s){
    if(s->tick_timer){ furi_timer_free(s->tick_timer); s->tick_timer = NULL; }
    if(s->off_timer){  furi_timer_free(s->off_timer);  s->off_timer  = NULL; }
}
static void start_tick_timer_if_needed(AppState* s){
    stop_timers(s);
    s->remaining_ms = 0;
    s->timeout_expired = false;

    if(!s->powered) return;          /* only in powered menu */
    if(!s->limit_runtime) return;    /* unlimited => no timers */
    if(s->active == 0) return;       /* Stand by => no countdown */

    uint32_t secs = kModes[s->active].default_secs;
    if(secs == 0) return;

    s->remaining_ms = secs * 1000U;

    if(!s->tick_timer) s->tick_timer = furi_timer_alloc(tick_timer_cb, FuriTimerTypePeriodic, s);
    if(!s->off_timer)  s->off_timer  = furi_timer_alloc(off_timer_cb,  FuriTimerTypeOnce,     s);

    furi_timer_start(s->tick_timer, furi_ms_to_ticks(1000));
    furi_timer_start(s->off_timer,  furi_ms_to_ticks(s->remaining_ms));
}

/* ---------- Apply powered mode (Stand by / Low / Mid / Max) ---------- */
static void apply_mode(AppState* s, uint8_t idx){
    if(idx >= MODE_COUNT) return;
    s->active = idx;

    const Mode* m = &kModes[idx];

    if(m->freq_hz == 0){
        /* Stand by: stop PWM and actively hold LOW (safe) */
        pwm_hw_stop_safe(&s->pwm_running);
        pin_to_pp_low();
        stop_timers(s);
        s->remaining_ms = 0;
        s->timeout_expired = false;
    } else {
        /* PWM run */
        pwm_hw_stop_safe(&s->pwm_running);
        pwm_hw_start_safe(m->freq_hz, &s->pwm_running);
        start_tick_timer_if_needed(s);
    }
    led_apply(s, m->led_blink_hz);
}

/* ---------- Back-hint timer ---------- */
static void hint_timer_cb(void* ctx){
    AppState* s = ctx;
    s->hint_visible = false;
    if(s->vp) view_port_update(s->vp);
}

/* ---------- Alerts ---------- */
static bool show_limit_alert_confirm(void){
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    DialogMessage* msg = dialog_message_alloc();

    dialog_message_set_header(msg, "Alert", 64, 2, AlignCenter, AlignTop);
    dialog_message_set_text(
        msg,
        "Long run without condenser\n"
        "and evaporator fans may\n"
        "damage compressor parts.",
        6, 16, AlignLeft, AlignTop);
    dialog_message_set_buttons(msg, "Cancel", NULL, "Confirm");

    DialogMessageButton res = dialog_message_show(dialogs, msg);

    dialog_message_free(msg);
    furi_record_close(RECORD_DIALOGS);
    return (res == DialogMessageButtonRight);
}

static bool show_power_on_confirm(void){
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    DialogMessage* msg = dialog_message_alloc();

    dialog_message_set_header(msg, "Alert", 64, 2, AlignCenter, AlignTop);
    dialog_message_set_text(
        msg,
        "Check your wiring!\n"
        "All pins will be activated!\n"
        "Check help!",
        64, 16, AlignCenter, AlignTop);
    dialog_message_set_buttons(msg, "Cancel", NULL, "Confirm");

    DialogMessageButton res = dialog_message_show(dialogs, msg);

    dialog_message_free(msg);
    furi_record_close(RECORD_DIALOGS);
    return (res == DialogMessageButtonRight);
}

/* ---------- Help layout (lines/limits) ---------- */
static inline void help_layout_params(uint8_t total_lines, uint8_t* out_max_lines, uint8_t* out_max_top_line){
    const uint8_t top = 10;
    const uint8_t line_h = 9;
    uint8_t ml = (uint8_t)((CANVAS_H - top) / line_h);
    if(ml < 1) ml = 1;
    uint8_t mtl = (total_lines > ml) ? (uint8_t)(total_lines - ml) : 0;
    if(out_max_lines) *out_max_lines = ml;
    if(out_max_top_line) *out_max_top_line = mtl;
}

/* ---------- Title helper ---------- */
static void draw_title(Canvas* c, const AppState* s){
    canvas_set_font(c, FontPrimary);
    canvas_set_color(c, ColorBlack);

    const char* inv_name = (s->inverter == InvEmbraco) ? "Embraco" : "Samsung";
    char title[32];
    snprintf(title, sizeof(title), "%s Starter", inv_name);
    canvas_draw_str(c, 4, TITLE_Y, title);

    /* right-aligned timer (if counting) with fixed margin from scrollbar */
    if(s->remaining_ms > 0){
        char tbuf[16];
        unsigned long sec = (unsigned long)((s->remaining_ms + 999)/1000);
        snprintf(tbuf, sizeof(tbuf), "%lus", sec);
        uint16_t w = canvas_string_width(c, tbuf);
        uint16_t right_x = (uint16_t)(SCROLLBAR_X - TIMER_MARGIN);
        uint16_t x = (w <= right_x) ? (uint16_t)(right_x - w) : 2;
        canvas_draw_str(c, x, TITLE_Y, tbuf);
    }
}

/* ---------- Draw: Select Inverter (initial screen) ---------- */
static void draw_select_inverter(Canvas* c, const AppState* s){
    canvas_clear(c);
    canvas_set_color(c, ColorBlack);

    /* Title */
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 4, TITLE_Y, "Inverter type");

    /* options */
    canvas_set_font(c, FontSecondary);
    int y = ROW_Y0;
    canvas_draw_str(c, 2, y, (s->cursor == 0) ? ">" : " ");
    canvas_draw_str(c, 14, y, "Embraco");
    y += ROW_DY;
    canvas_draw_str(c, 2, y, (s->cursor == 1) ? ">" : " ");
    canvas_draw_str(c, 14, y, "Samsung");

    /* scrollbar (2 items) */
    draw_scrollbar_dotted(c, 2, s->cursor);
}

/* ---------- Draw: Menu ---------- */
static void draw_menu(Canvas* c, const AppState* s){
    canvas_clear(c);
    draw_title(c, s);

    canvas_set_font(c, FontSecondary);
    const uint8_t MAX_ROWS = 4;

    /* Build dynamic list depending on powered flag */
    const bool powered = s->powered;
    uint8_t row_total = powered ? (uint8_t)(MODE_COUNT /*4*/ + 3 /*Power off + Settings + Help*/) 
                                : 3 /*Power on + Settings + Help*/;

    /* adjust first_visible to bounds */
    uint8_t first_visible = s->first_visible;
    if(first_visible + MAX_ROWS > row_total){
        first_visible = (row_total > MAX_ROWS) ? (uint8_t)(row_total - MAX_ROWS) : 0;
    }

    /* draw rows */
    for(uint8_t i = 0; i < MAX_ROWS; i++){
        uint8_t row = (uint8_t)(first_visible + i);
        if(row >= row_total) break;
        int y = ROW_Y0 + i*ROW_DY;

        if(row == s->cursor) canvas_draw_str(c, 2, y, ">");
        else canvas_draw_str(c, 2, y, " ");

        if(powered){
            /* indices: 0..3 => modes; 4 => Power off; 5 => Settings; 6 => Help */
            if(row < MODE_COUNT){
                canvas_draw_str(c, 14, y, kModes[row].name);
                if(row == s->active){
                    int check_x = (int)SCROLLBAR_X - TIMER_MARGIN - 10;
                    if(check_x < 90) check_x = 90;
                    draw_checkmark(c, check_x, y);
                }
            } else if(row == MODE_COUNT){
                canvas_draw_str(c, 14, y, "Power off");
            } else if(row == MODE_COUNT + 1){
                canvas_draw_str(c, 14, y, "Settings");
            } else {
                canvas_draw_str(c, 14, y, "Help");
            }
        } else {
            /* indices: 0 => Power on; 1 => Settings; 2 => Help */
            if(row == 0) canvas_draw_str(c, 14, y, "Power on");
            else if(row == 1) canvas_draw_str(c, 14, y, "Settings");
            else canvas_draw_str(c, 14, y, "Help");
        }
    }

    draw_scrollbar_dotted(c, row_total, s->cursor);

    /* bottom hint (short BACK): left-aligned to menu text (x=14) */
    if(s->hint_visible){
        const char* msg = "Long press back to exit";
        uint16_t text_h = 10;
        uint16_t text_y = (uint16_t)(CANVAS_H - 2);

        canvas_set_color(c, ColorBlack);
        canvas_draw_box(c, 0, (uint16_t)(text_y - text_h), CANVAS_W, (uint16_t)(text_h + 4));
        canvas_set_color(c, ColorWhite);
        canvas_draw_str(c, 14, text_y, msg);
        canvas_set_color(c, ColorBlack);
    }
}

/* ---------- Draw: Help (per inverter) ---------- */
static void draw_help(Canvas* c, const AppState* s){
    canvas_clear(c);
    canvas_set_font(c, FontSecondary);
    canvas_set_color(c, ColorBlack);

    const char* const* LINES = (s->inverter == InvEmbraco) ? HELP_EMBRACO : HELP_SAMSUNG;
    const uint8_t LINES_COUNT = (s->inverter == InvEmbraco) ? HELP_EMBRACO_COUNT : HELP_SAMSUNG_COUNT;

    uint8_t max_lines, max_top_line;
    help_layout_params(LINES_COUNT, &max_lines, &max_top_line);

    const uint8_t top = 10;
    const uint8_t line_h = 9;

    for(uint8_t i=0;i<max_lines;i++){
        uint8_t idx = (uint8_t)(s->help_top_line + i);
        if(idx >= LINES_COUNT) break;
        canvas_draw_str(c, 2, (uint8_t)(top + i*line_h), LINES[idx]);
    }

    /* scrollbar reflects top line (instant) */
    uint16_t total_steps = (uint16_t)(max_top_line + 1);
    if(total_steps < 1) total_steps = 1;
    draw_scrollbar_dotted(c, total_steps, s->help_top_line);
}

/* ---------- Draw: Settings ---------- */
/* Visual rows:
 * 0: "> Limit run time"   (selectable)
 * 1: "> Arrow captcha"    (selectable)
 * 2:   Inverter type      (header, non-selectable, aligned with title)
 * 3: "> Embraco"          (selectable)
 * 4: "> Samsung"          (selectable)
 */
static void draw_settings(Canvas* c, const AppState* s){
    canvas_clear(c);

    /* Title */
    canvas_set_font(c, FontPrimary);
    canvas_set_color(c, ColorBlack);
    canvas_draw_str(c, 4, TITLE_Y, "Settings");

    /* Body */
    canvas_set_font(c, FontSecondary);

    const uint8_t MAX_ROWS = 4;
    const uint8_t ROW_TOTAL = 5;

    uint8_t first_visible = s->first_visible;
    if(first_visible + MAX_ROWS > ROW_TOTAL){
        first_visible = (ROW_TOTAL > MAX_ROWS) ? (uint8_t)(ROW_TOTAL - MAX_ROWS) : 0;
    }

    for(uint8_t i=0;i<MAX_ROWS;i++){
        uint8_t row = (uint8_t)(first_visible + i);
        if(row >= ROW_TOTAL) break;
        int y = ROW_Y0 + i*ROW_DY;

        /* header "Inverter type" non-selectable (no caret) */
        if(row == 2){
            canvas_draw_str(c, 4, y, "Inverter type");
            continue;
        }

        /* caret for selectable rows */
        canvas_draw_str(c, 2, y, (s->cursor == row) ? ">" : " ");

        if(row == 0){
            canvas_draw_str(c, 14, y, "Limit run time");
            const char* val = s->limit_runtime ? "Yes" : "No";
            uint16_t w = canvas_string_width(c, val);
            uint16_t right_x = (uint16_t)(SCROLLBAR_X - TIMER_MARGIN);
            uint16_t x = (w <= right_x) ? (uint16_t)(right_x - w) : 2;
            canvas_draw_str(c, x, y, val);
        } else if(row == 1){
            canvas_draw_str(c, 14, y, "Arrow captcha");
            const char* val = s->arrow_captcha ? "Yes" : "No";
            uint16_t w = canvas_string_width(c, val);
            uint16_t right_x = (uint16_t)(SCROLLBAR_X - TIMER_MARGIN);
            uint16_t x = (w <= right_x) ? (uint16_t)(right_x - w) : 2;
            canvas_draw_str(c, x, y, val);
        } else if(row == 3){
            canvas_draw_str(c, 14, y, "Embraco");
            if(s->inverter == InvEmbraco){
                int check_x = (int)SCROLLBAR_X - TIMER_MARGIN - 10;
                if(check_x < 90) check_x = 90;
                draw_checkmark(c, check_x, y);
            }
        } else if(row == 4){
            canvas_draw_str(c, 14, y, "Samsung");
            if(s->inverter == InvSamsung){
                int check_x = (int)SCROLLBAR_X - TIMER_MARGIN - 10;
                if(check_x < 90) check_x = 90;
                draw_checkmark(c, check_x, y);
            }
        }
    }

    draw_scrollbar_dotted(c, ROW_TOTAL, s->cursor);
}

/* ---------- Draw dispatcher ---------- */
static void draw_cb(Canvas* c, void* ctx){
    AppState* s = ctx;
    switch(s->screen){
        case ScreenSelectInverter: draw_select_inverter(c, s); break;
        case ScreenMenu:           draw_menu(c, s); break;
        case ScreenHelp:           draw_help(c, s); break;
        case ScreenSettings:       draw_settings(c, s); break;
        default:                   draw_menu(c, s); break;
    }
}

/* ---------- Input queue plumbing ---------- */
typedef struct { FuriMessageQueue* q; } InputCtx;
static void vp_input_cb(InputEvent* e, void* ctx){
    InputCtx* ic = ctx;
    InputEvent ev = *e;
    furi_message_queue_put(ic->q, &ev, 0);
}

/* ---------- Power transitions ---------- */
static void enter_safe_menu(AppState* s){
    /* safe: disconnect line (Hi-Z), stop PWM/LED/timers, show minimal menu */
    s->powered = false;
    s->cursor = 0;
    s->first_visible = 0;

    pwm_hw_stop_safe(&s->pwm_running);
    pin_to_hiz();
    led_apply(s, 0);
    stop_timers(s);
    s->remaining_ms = 0;
    s->timeout_expired = false;
}

static void enter_powered_menu_standby(AppState* s){
    /* after confirmation: powered menu with Stand by selected */
    s->powered = true;
    s->cursor = 0;                 /* caret on "Stand by" */
    s->first_visible = 0;
    apply_mode(s, 0);              /* Stand by — PP LOW, no timer */
}

/* ---------- Main ---------- */
int32_t embraco_starter(void* p){
    UNUSED(p);

    AppState s = {
        .screen = ScreenSelectInverter, /* по ТЗ — сначала выбор инвертора */
        .inverter = InvEmbraco,         /* default; изменится, если выберут Samsung */
        .powered = false,               /* в начале безопасное состояние */
        .cursor = 0,
        .first_visible = 0,
        .active = 0,
        .help_top_line = 0,
        .limit_runtime = true,
        .arrow_captcha = true,          /* по умолчанию Yes */
        .notif = furi_record_open(RECORD_NOTIFICATION),
        .led_timer = NULL,
        .led_on = false,
        .pwm_running = false,
        .hint_visible = false,
        .hint_timer = NULL,
        .tick_timer = NULL,
        .off_timer = NULL,
        .remaining_ms = 0,
        .timeout_expired = false,
        .gui = NULL,
        .vp = NULL,
        .q = NULL,
    };

    s.gui = furi_record_open(RECORD_GUI);
    s.vp = view_port_alloc();
    s.q  = furi_message_queue_alloc(8, sizeof(InputEvent));
    InputCtx ic = {.q = s.q};

    view_port_draw_callback_set(s.vp, draw_cb, &s);
    view_port_input_callback_set(s.vp, vp_input_cb, &ic);
    gui_add_view_port(s.gui, s.vp, GuiLayerFullscreen);

    /* absolute safety at start */
    pin_to_hiz();
    led_apply(&s, 0);

    const uint8_t MAX_ROWS = 4;

    bool exit_app = false;
    InputEvent ev;

    while(!exit_app){
        /* service timeout event on main loop (from off_timer) */
        if(s.timeout_expired){
            s.timeout_expired = false;
            /* auto switch to Stand by (not full Power off) when time expires */
            enter_powered_menu_standby(&s);
            view_port_update(s.vp);
        }

        if(furi_message_queue_get(s.q, &ev, 100) == FuriStatusOk){
            /* Long BACK anywhere => exit app */
            if(ev.type == InputTypeLong && ev.key == InputKeyBack){
                exit_app = true;
                view_port_update(s.vp);
                continue;
            }

            switch(s.screen){
                /* -------- Initial inverter selection -------- */
                case ScreenSelectInverter: {
                    if(ev.type == InputTypeShort || ev.type == InputTypeRepeat){
                        if(ev.key == InputKeyUp){
                            s.cursor = (s.cursor == 0) ? 1 : 0;
                        } else if(ev.key == InputKeyDown){
                            s.cursor = (s.cursor == 1) ? 0 : 1;
                        } else if(ev.key == InputKeyOk){
                            /* apply selection and go to SAFE MENU immediately */
                            s.inverter = (s.cursor == 0) ? InvEmbraco : InvSamsung;
                            enter_safe_menu(&s);
                            s.screen = ScreenMenu;
                        } else if(ev.key == InputKeyBack){
                            /* show hint; require long press to exit */
                            s.hint_visible = true;
                            if(!s.hint_timer){
                                s.hint_timer = furi_timer_alloc(hint_timer_cb, FuriTimerTypeOnce, &s);
                            }
                            furi_timer_start(s.hint_timer, furi_ms_to_ticks(1500));
                        }
                    }
                } break;

                /* -------- Main menu -------- */
                case ScreenMenu: {
                    /* determine dynamic row_total for navigation */
                    const bool powered = s.powered;
                    uint8_t row_total = powered ? (uint8_t)(MODE_COUNT + 3) : 3;

                    if(ev.type == InputTypeShort){
                        if(ev.key == InputKeyUp){
                            if(s.cursor == 0){
                                s.cursor = (uint8_t)(row_total - 1);
                                s.first_visible = (row_total > MAX_ROWS) ? (uint8_t)(row_total - MAX_ROWS) : 0;
                            } else {
                                s.cursor--;
                                if(s.cursor < s.first_visible) s.first_visible = s.cursor;
                            }
                        } else if(ev.key == InputKeyDown){
                            if(s.cursor == (uint8_t)(row_total - 1)){
                                s.cursor = 0;
                                s.first_visible = 0;
                            } else {
                                s.cursor++;
                                if(s.cursor >= s.first_visible + MAX_ROWS){
                                    s.first_visible = (uint8_t)(s.cursor - (MAX_ROWS - 1));
                                }
                            }
                        } else if(ev.key == InputKeyOk){
                            if(powered){
                                /* 0..3 => modes, 4 => Power off, 5 => Settings, 6 => Help */
                                if(s.cursor < MODE_COUNT){
                                    apply_mode(&s, s.cursor);
                                } else if(s.cursor == MODE_COUNT){
                                    /* Power off: go to SAFE MENU (Hi-Z) and shrink list */
                                    enter_safe_menu(&s);
                                } else if(s.cursor == MODE_COUNT + 1){
                                    s.screen = ScreenSettings;
                                    s.cursor = 0;
                                    s.first_visible = 0;
                                } else {
                                    /* Help: switch to Stand by (PP LOW), stop timers via apply_mode(0) and show help */
                                    apply_mode(&s, 0); /* Stand by: PP LOW, no countdown */
                                    s.screen = ScreenHelp;
                                    s.help_top_line = 0;
                                }
                            } else {
                                /* 0 => Power on (show alert), 1 => Settings, 2 => Help */
                                if(s.cursor == 0){
                                    if(show_power_on_confirm()){
                                        enter_powered_menu_standby(&s);
                                    }
                                } else if(s.cursor == 1){
                                    s.screen = ScreenSettings;
                                    s.cursor = 0;
                                    s.first_visible = 0;
                                } else {
                                    s.screen = ScreenHelp;
                                    s.help_top_line = 0;
                                }
                            }
                        } else if(ev.key == InputKeyBack){
                            /* short back => hint (left-aligned) */
                            s.hint_visible = true;
                            if(!s.hint_timer){
                                s.hint_timer = furi_timer_alloc(hint_timer_cb, FuriTimerTypeOnce, &s);
                            }
                            furi_timer_start(s.hint_timer, furi_ms_to_ticks(1500));
                        }
                    }
                } break;

                /* -------- Help -------- */
                case ScreenHelp: {
                    if(ev.type == InputTypeShort || ev.type == InputTypeRepeat){
                        const uint8_t total_lines = (s.inverter == InvEmbraco) ? HELP_EMBRACO_COUNT : HELP_SAMSUNG_COUNT;
                        uint8_t max_lines, max_top_line;
                        help_layout_params(total_lines, &max_lines, &max_top_line);

                        if(ev.key == InputKeyUp){
                            if(s.help_top_line > 0) s.help_top_line--;
                        } else if(ev.key == InputKeyDown){
                            if(s.help_top_line < max_top_line) s.help_top_line++;
                        } else if(ev.key == InputKeyBack){
                            s.screen = ScreenMenu;
                        }
                    }
                } break;

                /* -------- Settings -------- */
                case ScreenSettings: {
                    const uint8_t ROW_TOTAL = 5;
                    const uint8_t MAX_ROWS_S = 4;

                    if(ev.type == InputTypeShort){
                        if(ev.key == InputKeyUp){
                            if(s.cursor == 0){
                                s.cursor = (uint8_t)(ROW_TOTAL - 1);
                                s.first_visible = (ROW_TOTAL > MAX_ROWS_S) ? (uint8_t)(ROW_TOTAL - MAX_ROWS_S) : 0;
                            } else {
                                s.cursor--;
                                if(s.cursor == 2) s.cursor = 1; /* skip header */
                                if(s.cursor < s.first_visible) s.first_visible = s.cursor;
                            }
                        } else if(ev.key == InputKeyDown){
                            if(s.cursor == (uint8_t)(ROW_TOTAL - 1)){
                                s.cursor = 0;
                                s.first_visible = 0;
                            } else {
                                s.cursor++;
                                if(s.cursor == 2) s.cursor = 3; /* skip header */
                                if(s.cursor >= s.first_visible + MAX_ROWS_S){
                                    s.first_visible = (uint8_t)(s.cursor - (MAX_ROWS_S - 1));
                                }
                            }
                        } else if(ev.key == InputKeyOk){
                            if(s.cursor == 0){
                                /* Limit run time toggle with alert on Yes->No */
                                if(s.limit_runtime){
                                    if(show_limit_alert_confirm()){
                                        s.limit_runtime = false;
                                        /* cancel timers immediately */
                                        stop_timers(&s);
                                        s.remaining_ms = 0;
                                    }
                                } else {
                                    s.limit_runtime = true;
                                    start_tick_timer_if_needed(&s);
                                }
                            } else if(s.cursor == 1){
                                /* Arrow captcha toggle (placeholder) */
                                s.arrow_captcha = !s.arrow_captcha;
                            } else if(s.cursor == 3){
                                /* Choose Embraco — if already selected, do nothing */
                                if(s.inverter != InvEmbraco){
                                    s.inverter = InvEmbraco;
                                    /* Return to SAFE MENU with 3 items and updated title */
                                    enter_safe_menu(&s);
                                    s.screen = ScreenMenu;
                                }
                            } else if(s.cursor == 4){
                                /* Choose Samsung */
                                if(s.inverter != InvSamsung){
                                    s.inverter = InvSamsung;
                                    enter_safe_menu(&s);
                                    s.screen = ScreenMenu;
                                }
                            }
                        } else if(ev.key == InputKeyBack){
                            s.screen = ScreenMenu;
                            s.cursor = 0;
                            s.first_visible = 0;
                        }
                    }
                } break;
            } /* switch(screen) */

            view_port_update(s.vp);
        } /* queue ok */
    } /* while */

    /* ---------- Cleanup ---------- */
    if(s.led_timer){ furi_timer_stop(s.led_timer); furi_timer_free(s.led_timer); s.led_timer = NULL; }
    if(s.hint_timer){ furi_timer_stop(s.hint_timer); furi_timer_free(s.hint_timer); s.hint_timer = NULL; }
    stop_timers(&s);
    free_timers(&s);
    pwm_hw_stop_safe(&s.pwm_running);
    pin_to_hiz();
    notification_message(s.notif, &sequence_reset_rgb);
    furi_record_close(RECORD_NOTIFICATION);

    gui_remove_view_port(s.gui, s.vp);
    view_port_free(s.vp);
    furi_message_queue_free(s.q);
    furi_record_close(RECORD_GUI);
    return 0;
}
