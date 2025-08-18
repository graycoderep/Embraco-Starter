#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <input/input.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <stdbool.h>

/*** Wiring:
 *  + signal: PA7 (external pin "2 (A7)")
 *  - GND:    pin "8 (GND)")
***/
static const GpioPin* PWM_PIN = &gpio_ext_pa7;

/* ===== Helpers for pin states ===== */
static inline void pin_to_hiz(void){
    // Hi‑Z (без подтяжек)
    furi_hal_gpio_init(PWM_PIN, GpioModeInput, GpioPullNo, GpioSpeedLow);
}
static inline void pin_to_pp_low(void){
    furi_hal_gpio_init(PWM_PIN, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);
    furi_hal_gpio_write(PWM_PIN, false);
}

/* ===== Hardware PWM (PA7) ===== */
#define PWM_CH FuriHalPwmOutputIdTim1PA7

static inline void pwm_hw_stop_safe(bool* running){
    if(running && *running){
        furi_hal_pwm_stop(PWM_CH);
        // Дать периферии «отщелкнуться», прежде чем трогать GPIO
        furi_delay_ms(1);
        *running = false;
    }
}

static inline void pwm_hw_start_safe(uint32_t freq_hz, bool* running){
    // 50% duty; частота держится «в железе»
    furi_hal_pwm_start(PWM_CH, freq_hz, 50);
    if(running) *running = true;
}

/* ===== Modes (Power off first) ===== */
typedef struct {
    const char* name;
    uint32_t freq_hz;       // 0 = off
    uint8_t led_blink_hz;   // 0/1/2/4
} Mode;

static const Mode kModes[] = {
    {"Power off", 0,   0},
    {"Low speed", 55,  1},
    {"Mid speed", 100, 2},
    {"Max speed", 160, 4},
};
#define MODE_COUNT (sizeof(kModes)/sizeof(kModes[0]))

/* ===== Help text ===== */
static const char* HELP_LINES[] = {
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
#define HELP_LINES_COUNT (sizeof(HELP_LINES)/sizeof(HELP_LINES[0]))

/* ===== App state ===== */
typedef struct {
    // menu navigation
    uint8_t cursor;        // 0..MODE_COUNT (MODE_COUNT == Help item)
    uint8_t first_visible; // top row index in 4-line window
    uint8_t active;        // applied mode index (0..MODE_COUNT-1)
    bool in_help;
    uint8_t help_top_line;

    // LED blink
    NotificationApp* notif;
    FuriTimer* led_timer;
    bool led_on;

    // HW PWM state
    bool pwm_running;

    // IO
    Gui* gui;
    ViewPort* vp;
    FuriMessageQueue* q;
} AppState;

/* ===== LED control ===== */
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

    uint32_t ms = 1000U / (blink_hz * 2U); // полупериод мигания
    if(ms == 0) ms = 1;
    s->led_timer = furi_timer_alloc(led_timer_cb, FuriTimerTypePeriodic, s);
    furi_timer_start(s->led_timer, furi_ms_to_ticks(ms));
}

/* ===== Apply mode ===== */
static void apply_mode(AppState* s, uint8_t idx){
    if(idx >= MODE_COUNT) return;
    s->active = idx;

    const Mode* m = &kModes[idx];

    if(m->freq_hz == 0){
        // Полное выключение генерации
        pwm_hw_stop_safe(&s->pwm_running);
        pin_to_pp_low(); // безопасная нагрузка на «0» (не Hi‑Z)
    }else{
        // Гарантированно останавливаем прошлый режим, затем стартуем новый
        pwm_hw_stop_safe(&s->pwm_running);
        pwm_hw_start_safe(m->freq_hz, &s->pwm_running);
    }
    led_apply(s, m->led_blink_hz);
}

/* ===== UI ===== */
static void draw_menu(Canvas* c, const AppState* s){
    canvas_clear(c);

    // Заголовок жирным, слева
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 4, 14, "Embraco Starter");

    // Список: 4 строки, точка справа у активного
    canvas_set_font(c, FontSecondary);
    const int y0 = 26;            // 26,38,50,62
    const int dy = 12;
    const uint8_t max_rows = 4;
    const uint8_t total_rows = (uint8_t)(MODE_COUNT + 1); // + Help

    uint8_t first_visible = s->first_visible;
    if(first_visible + max_rows > total_rows){
        first_visible = (total_rows > max_rows) ? (uint8_t)(total_rows - max_rows) : 0;
    }

    for(uint8_t i=0;i<max_rows;i++){
        uint8_t row = (uint8_t)(first_visible + i);
        if(row >= total_rows) break;
        int y = y0 + i*dy;

        if(row == s->cursor) canvas_draw_str(c, 2, y, ">");

        if(row < MODE_COUNT){
            canvas_draw_str(c, 14, y, kModes[row].name);
            if(row == s->active) canvas_draw_box(c, 120, y-4, 4, 4); // точка справа
        }else{
            canvas_draw_str(c, 14, y, "Help");
        }
    }
}

static void draw_help(Canvas* c, const AppState* s){
    canvas_clear(c);
    canvas_set_font(c, FontSecondary);

    // 6 строк без обрезаний
    const uint8_t top = 10;
    const uint8_t line_h = 9;
    uint8_t max_lines = (64 - top) / line_h; // обычно 6
    if(max_lines < 1) max_lines = 1;
    if(max_lines > 6) max_lines = 6;

    uint8_t max_top_line = 0;
    if(HELP_LINES_COUNT > max_lines) max_top_line = (uint8_t)(HELP_LINES_COUNT - max_lines);
    if(s->help_top_line > max_top_line) ((AppState*)s)->help_top_line = max_top_line;

    for(uint8_t i=0;i<max_lines;i++){
        uint8_t idx = (uint8_t)(s->help_top_line + i);
        if(idx >= HELP_LINES_COUNT) break;
        canvas_draw_str(c, 2, top + i*line_h, HELP_LINES[idx]);
    }
}

static void draw_cb(Canvas* c, void* ctx){
    AppState* s = ctx;
    if(s->in_help) draw_help(c, s);
    else draw_menu(c, s);
}

/* ===== Input plumbing (queue) ===== */
typedef struct { FuriMessageQueue* q; } InputCtx;
static void vp_input_cb(InputEvent* e, void* ctx){
    InputCtx* ic = ctx;
    // Копируем событие, не кладём указатель
    InputEvent ev = *e;
    furi_message_queue_put(ic->q, &ev, 0);
}

/* ===== Main ===== */
int32_t embraco_starter(void* p){
    UNUSED(p);

    AppState s = {
        .cursor = 0,
        .first_visible = 0,
        .active = 0,
        .in_help = true,          // стартуем в Help
        .help_top_line = 0,
        .notif = furi_record_open(RECORD_NOTIFICATION),
        .led_timer = NULL,
        .led_on = false,
        .pwm_running = false,
        .gui = NULL,
        .vp = NULL,
        .q = NULL,
    };

    s.gui = furi_record_open(RECORD_GUI);
    s.vp = view_port_alloc();
    s.q = furi_message_queue_alloc(8, sizeof(InputEvent));
    InputCtx ic = {.q = s.q};

    view_port_draw_callback_set(s.vp, draw_cb, &s);
    view_port_input_callback_set(s.vp, vp_input_cb, &ic);
    gui_add_view_port(s.gui, s.vp, GuiLayerFullscreen);

    /* Стартовая безопасность: Hi‑Z, никакого PWM, LED off */
    pin_to_hiz();
    led_apply(&s, 0);

    const uint8_t max_rows = 4;
    const uint8_t total_rows = (uint8_t)(MODE_COUNT + 1);

    bool exit = false;
    InputEvent ev;
    while(!exit){
        if(furi_message_queue_get(s.q, &ev, 100) == FuriStatusOk){
            if(s.in_help){
                if(ev.type == InputTypeShort || ev.type == InputTypeRepeat){
                    if(ev.key == InputKeyUp){
                        if(s.help_top_line>0) s.help_top_line--;
                    }else if(ev.key == InputKeyDown){
                        const uint8_t top = 10, line_h = 9;
                        uint8_t max_lines = (64 - top) / line_h; if(max_lines<1) max_lines=1; if(max_lines>6) max_lines=6;
                        uint8_t top_limit = 0;
                        if(HELP_LINES_COUNT > max_lines) top_limit = (uint8_t)(HELP_LINES_COUNT - max_lines);
                        if(s.help_top_line < top_limit) s.help_top_line++;
                    }else if(ev.key == InputKeyBack){
                        // Выход из Help -> главное меню, сразу Power off
                        s.in_help = false;
                        s.cursor = 0;
                        s.first_visible = 0;
                        apply_mode(&s, 0); // safe: stop if running, PP LOW
                    }
                }
            }else{
                if(ev.type == InputTypeShort){
                    if(ev.key == InputKeyUp){
                        if(s.cursor == 0){
                            s.cursor = (uint8_t)(total_rows - 1);
                            s.first_visible = (total_rows > max_rows) ? (uint8_t)(total_rows - max_rows) : 0;
                        }else{
                            s.cursor--;
                            if(s.cursor < s.first_visible) s.first_visible = s.cursor;
                        }
                    }else if(ev.key == InputKeyDown){
                        if(s.cursor == (uint8_t)(total_rows - 1)){
                            s.cursor = 0;
                            s.first_visible = 0;
                        }else{
                            s.cursor++;
                            if(s.cursor >= s.first_visible + max_rows){
                                s.first_visible = (uint8_t)(s.cursor - (max_rows - 1));
                            }
                        }
                    }else if(ev.key == InputKeyOk){
                        if(s.cursor < MODE_COUNT){
                            apply_mode(&s, s.cursor);
                        }else{
                            // Вход в Help из меню: стоп PWM (если шёл) → Hi‑Z → LED off
                            pwm_hw_stop_safe(&s.pwm_running);
                            pin_to_hiz();
                            led_apply(&s, 0);
                            s.in_help = true;
                            s.help_top_line = 0;
                            s.active = 0; // визуально: "Power off" активен
                        }
                    }else if(ev.key == InputKeyBack){
                        exit = true;
                    }
                }
            }
            view_port_update(s.vp);
        }
    }

    /* ===== Cleanup: стоп PWM, Hi‑Z, сброс LED ===== */
    if(s.led_timer){
        furi_timer_stop(s.led_timer);
        furi_timer_free(s.led_timer);
        s.led_timer = NULL;
    }
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
