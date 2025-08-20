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

/*** Wiring / Подключение:
 *  + signal: PA7 (external pin "2 (A7)")
 *  - GND:    pin "8 (GND)")
 *  Важно: приложение управляет инверторным компрессором через аппаратный PWM на PA7.
***/
static const GpioPin* PWM_PIN = &gpio_ext_pa7;

/* Геометрия скроллбара и таймера (фиксированные значения под экран 128x64) */
enum {
    SCROLLBAR_X = 124,   /* X-координата пунктирной дорожки справа */
    SCROLLBAR_W = 3,     /* ширина бегунка (ползунка) скроллбара */
    TIMER_MARGIN = 6,    /* фиксированный отступ таймера от правого края скроллбара */
};

/* ===== GPIO helpers / Вспомогательные функции для безопасных состояний ножки ===== */
static inline void pin_to_hiz(void){
    /* Переводим ножку в Hi‑Z (вход без подтяжек) — электрически безопасное состояние. */
    furi_hal_gpio_init(PWM_PIN, GpioModeInput, GpioPullNo, GpioSpeedLow);
}
static inline void pin_to_pp_low(void){
    /* Выставляем "0" push‑pull — безопасная нагрузка в «низ». */
    furi_hal_gpio_init(PWM_PIN, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);
    furi_hal_gpio_write(PWM_PIN, false);
}

/* ===== Hardware PWM (PA7) ===== */
#define PWM_CH FuriHalPwmOutputIdTim1PA7

static inline void pwm_hw_stop_safe(bool* running){
    /* Аккуратная остановка PWM: сначала стоп, короткая задержка, снимем флаг. */
    if(running && *running){
        furi_hal_pwm_stop(PWM_CH);
        furi_delay_ms(1); /* дать периферии «успокоиться» */
        *running = false;
    }
}

static inline void pwm_hw_start_safe(uint32_t freq_hz, bool* running){
    /* Запуск PWM на заданной частоте с 50% duty (по требованиям). */
    furi_hal_pwm_start(PWM_CH, freq_hz, 50); // duty 50%
    if(running) *running = true;
}

/* ===== Modes / Режимы работы с дефолтными таймаутами (при Limit=Yes) ===== */
typedef struct {
    const char* name;       /* отображаемое имя в меню */
    uint32_t freq_hz;       /* частота PWM; 0 = выключено */
    uint8_t led_blink_hz;   /* мигание RGB-индикатора (0 — выкл.) */
    uint32_t default_secs;  /* дефолтный лимит работы в секундах */
} Mode;

/* Порядок важен — индексы используются в меню:
 * 0 — Power off; 1 — Low; 2 — Mid; 3 — Max
 */
static const Mode kModes[] = {
    {"Power off", 0,   0,   0},   // 0 — выключено
    {"Low speed", 55,  1, 120},   // 1 — 2 мин
    {"Mid speed", 100, 2,  60},   // 2 — 1 мин
    {"Max speed", 160, 4,  30},   // 3 — 30 сек
};
#define MODE_COUNT (sizeof(kModes)/sizeof(kModes[0]))

/* ===== Help text (без заголовка, выравнивается кодом отступом сверху) ===== */
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

/* ===== Screens / Экраны приложения ===== */
typedef enum {
    ScreenMenu = 0,   /* основное меню режимов + Settings + Help */
    ScreenHelp,       /* подробная помощь (скролл-текст) */
    ScreenSettings,   /* настройки (один параметр: Limit run time) */
} ScreenId;

/* ===== App state / Состояние всего приложения ===== */
typedef struct {
    /* Какой экран показываем */
    ScreenId screen;

    /* Навигация по списку на главном экране */
    uint8_t cursor;         // индекс текущей строки (0..ROW_TOTAL-1)
    uint8_t first_visible;  // верхняя видимая строка в окне из 4 строк
    uint8_t active;         // применённый режим (0..MODE_COUNT-1)

    /* Прокрутка экрана Help */
    uint8_t help_top_line;  // индекс верхней видимой строки в Help

    /* Настройка ограничения времени работы */
    bool limit_runtime;     // Yes (true) / No (false)

    /* Индикация (зелёный светодиод) */
    NotificationApp* notif;
    FuriTimer* led_timer;
    bool led_on;

    /* Состояние аппаратного PWM */
    bool pwm_running;

    /* Всплывающая подсказка про долгий Back */
    bool hint_visible;
    FuriTimer* hint_timer;

    /* Таймеры времени работы: один на UI-тиканье, второй — точный one-shot для авто-OFF */
    FuriTimer* tick_timer;      // 1 Гц для обновления секунд на экране
    FuriTimer* off_timer;       // одновыстрел для точного выключения по лимиту
    uint32_t remaining_ms;      // сколько осталось (для отображения), мс
    bool timeout_expired;       // событие «время вышло» для главного цикла

    /* IO: GUI/ViewPort/очередь */
    Gui* gui;
    ViewPort* vp;
    FuriMessageQueue* q;
} AppState;

/* ===== LED control / Управление зелёной индикацией ===== */
static void led_set(NotificationApp* n, bool on){
    if(!n) return;
    if(on) notification_message(n, &sequence_set_green_255);
    else   notification_message(n, &sequence_reset_rgb);
}

static void led_timer_cb(void* ctx){
    /* Периодическое мигание LED с частотой, заданной в режиме. */
    AppState* s = ctx;
    s->led_on = !s->led_on;
    led_set(s->notif, s->led_on);
}

static void led_apply(AppState* s, uint8_t blink_hz){
    /* Перенастройка мигания под текущий режим. 0 — выключить. */
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

/* ===== Dotted scrollbar (Momentum-style) / Пунктирный скроллбар =====
 * total_steps — количество дискретных позиций (элементов).
 * pos         — текущая позиция (0..total_steps-1), бегунок мгновенно следует за курсором.
 */
static void draw_scrollbar_dotted(Canvas* c, uint16_t total_steps, uint16_t pos){
    if(total_steps <= 1) return;

    const uint16_t x = SCROLLBAR_X;  // вертикальная пунктирная «дорожка»
    const uint16_t y0 = 2;           // верхняя точка пунктира
    const uint16_t y1 = 62;          // нижняя точка пунктира

    /* Рисуем пунктир: точка каждые 3px. */
    for(uint16_t y = y0; y <= y1; y += 3){
        canvas_draw_dot(c, x, y);
    }

    /* Вычисляем положение центра бегунка по текущей позиции.
     * Трек (длина) = y1 - y0. Делим на (total_steps - 1), чтобы крайние позиции были на краях.
     */
    const uint16_t track_h = (uint16_t)(y1 - y0);
    uint16_t denom = (uint16_t)((total_steps > 1) ? (total_steps - 1) : 1);
    uint16_t thumb_y = y0 + (uint16_t)((pos * track_h) / denom);

    /* Важный финт: «упираем» бегунок в самый низ экрана, наезжая на последнюю точку пунктира.
     * При высоте бегунка 4px его нижняя граница окажется внизу (Y≈63), что выглядит «эстетично».
     */
    if(thumb_y < y0) thumb_y = y0;
    if(thumb_y > (uint16_t)(y1 - 1)) thumb_y = (uint16_t)(y1 - 1);

    /* Сам бегунок: прямоугольник 3x4, центрированный по пунктиру. */
    canvas_draw_box(c, (uint16_t)(x - 1), (uint16_t)(thumb_y - 1), 3, 4);
}

/* ===== Рисуем аккуратную галочку статуса (7x7), baseline_y — базовая линия текста ===== */
static void draw_checkmark(Canvas* c, int x, int baseline_y){
    /* Галочка из двух линий: короткая вниз-вправо, длинная вверх-вправо */
    int y = baseline_y - 5;
    canvas_draw_line(c, x,     y+3, x+2, y+5);
    canvas_draw_line(c, x+2,   y+5, x+7, y   );
}

/* ===== Countdown: 1 Гц тиканье (UI) и точный one-shot (авто‑OFF) ===== */
static void tick_timer_cb(void* ctx){
    /* Тикаем раз в секунду: уменьшаем оставшееся время для отображения. */
    AppState* s = ctx;
    if(s->remaining_ms >= 1000) s->remaining_ms -= 1000;
    else s->remaining_ms = 0;
    if(s->vp) view_port_update(s->vp);
}

static void off_timer_cb(void* ctx){
    /* Ровно по окончании лимита — сигнал в главный цикл выключить режим. */
    AppState* s = ctx;
    s->remaining_ms = 0;
    s->timeout_expired = true;
    if(s->vp) view_port_update(s->vp);
}

static void stop_timers(AppState* s){
    /* Останавливаем оба таймера (без освобождения). */
    if(s->tick_timer) furi_timer_stop(s->tick_timer);
    if(s->off_timer)  furi_timer_stop(s->off_timer);
}

static void free_timers(AppState* s){
    /* Безопасно освобождаем оба таймера. */
    if(s->tick_timer){ furi_timer_free(s->tick_timer); s->tick_timer = NULL; }
    if(s->off_timer){  furi_timer_free(s->off_timer);  s->off_timer = NULL; }
}

static void start_tick_timer_if_needed(AppState* s){
    /* Старт/рестарт таймеров для режима, с учётом настройки Limit. */
    stop_timers(s);
    s->remaining_ms = 0;
    s->timeout_expired = false;

    if(!s->limit_runtime) return;     // без лимита — таймеры не запускаем
    if(s->active == 0) return;        // Power off — отсчитывать нечего
    uint32_t secs = kModes[s->active].default_secs;
    if(secs == 0) return;             // на всякий случай

    s->remaining_ms = secs * 1000U;

    if(!s->tick_timer) s->tick_timer = furi_timer_alloc(tick_timer_cb, FuriTimerTypePeriodic, s);
    if(!s->off_timer)  s->off_timer  = furi_timer_alloc(off_timer_cb,  FuriTimerTypeOnce,     s);

    /* Тикаем раз в секунду для UI, а one‑shot выключит ровно по лимиту (без «+1с» лагов). */
    furi_timer_start(s->tick_timer, furi_ms_to_ticks(1000));
    furi_timer_start(s->off_timer,  furi_ms_to_ticks(s->remaining_ms));
}

/* ===== Apply mode / Применение выбранного режима ===== */
static void apply_mode(AppState* s, uint8_t idx){
    /* Безопасно переключаемся между частотами: стоп → старт, а для OFF — тянем «вниз». */
    if(idx >= MODE_COUNT) return;
    s->active = idx;

    const Mode* m = &kModes[idx];

    if(m->freq_hz == 0){
        /* Выключение: стоп PWM, безопасный «0» на пине, сброс таймеров. */
        pwm_hw_stop_safe(&s->pwm_running);
        pin_to_pp_low();
        stop_timers(s);
        s->remaining_ms = 0;
        s->timeout_expired = false;
    }else{
        /* Включение новой частоты: сначала стоп предыдущего PWM, затем старт нового. */
        pwm_hw_stop_safe(&s->pwm_running);
        pwm_hw_start_safe(m->freq_hz, &s->pwm_running);
        start_tick_timer_if_needed(s);
    }
    /* Подстраиваем мигание индикатора под режим. */
    led_apply(s, m->led_blink_hz);
}

/* ===== Hint timer / Таймер скрытия подсказки про длинный Back ===== */
static void hint_timer_cb(void* ctx){
    AppState* s = ctx;
    s->hint_visible = false;
    if(s->vp) view_port_update(s->vp);
}

/* ===== Alert / Подтверждение отключения лимита времени =====
 * Нативный диалог Flipper: заголовок по центру, текст с отступом, кнопки Cancel/Confirm по краям.
 */
static bool show_limit_alert_confirm(void){
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    DialogMessage* msg = dialog_message_alloc();

    /* Заголовок: аккуратный отступ сверху (y=2) и по центру. */
    dialog_message_set_header(msg, "Alert", 64, 2, AlignCenter, AlignTop);

    /* Короткий текст предупреждения, приподнят над кнопками, чтобы не липнуть к низу. */
    dialog_message_set_text(
        msg,
        "Long run without condenser\n"
        "and evaporator fans may\n"
        "damage compressor parts.",
        6, 16, AlignLeft, AlignTop);

    /* Кнопки: Cancel слева, Confirm справа. Центральная кнопка не используется (NULL). */
    dialog_message_set_buttons(msg, "Cancel", NULL, "Confirm");

    DialogMessageButton res = dialog_message_show(dialogs, msg);

    dialog_message_free(msg);
    furi_record_close(RECORD_DIALOGS);
    return (res == DialogMessageButtonRight);
}

/* ===== Help layout helpers / Вспомогательное вычисление параметров раскладки Help ===== */
static inline void help_layout_params(uint8_t* out_max_lines, uint8_t* out_max_top_line){
    /* Гарантируем одинаковое вычисление доступных строк и последнего «top» по всему коду. */
    const uint8_t top = 10;    /* отступ сверху, чтобы текст не прилипал к рамке дисплея */
    const uint8_t line_h = 9;  /* высота строки шрифта FontSecondary */
    uint8_t ml = (uint8_t)((64 - top) / line_h);
    if(ml < 1) ml = 1;
    uint8_t mtl = (HELP_LINES_COUNT > ml) ? (uint8_t)(HELP_LINES_COUNT - ml) : 0;
    if(out_max_lines) *out_max_lines = ml;
    if(out_max_top_line) *out_max_top_line = mtl;
}

/* ===== UI: Menu / Главный экран ===== */
static void draw_menu(Canvas* c, const AppState* s){
    canvas_clear(c);

    /* Заголовок (слева) */
    canvas_set_font(c, FontPrimary);
    canvas_set_color(c, ColorBlack);
    canvas_draw_str(c, 4, 14, "Embraco Starter");

    /* Таймер (секунды), выровнен по правой стороне с фиксированным отступом от скроллбара. */
    if(s->remaining_ms > 0){
        char tbuf[16];
        unsigned long sec = (unsigned long)((s->remaining_ms + 999)/1000);
        snprintf(tbuf, sizeof(tbuf), "%lus", sec);
        uint16_t w = canvas_string_width(c, tbuf);
        uint16_t right_x = (uint16_t)(SCROLLBAR_X - TIMER_MARGIN);
        uint16_t x = (w <= right_x) ? (uint16_t)(right_x - w) : 2;
        canvas_draw_str(c, x, 14, tbuf);
    }

    /* Список: 4 видимые строки, курсор «>», активный режим помечен галочкой справа. */
    canvas_set_font(c, FontSecondary);
    const int y0 = 26;            // строки на Y: 26, 38, 50, 62
    const int dy = 12;

    const uint8_t ROW_TOTAL = MODE_COUNT + 2; /* режимы + Settings + Help */
    uint8_t first_visible = s->first_visible;
    if(first_visible + 4 > ROW_TOTAL){
        first_visible = (ROW_TOTAL > 4) ? (uint8_t)(ROW_TOTAL - 4) : 0;
    }

    for(uint8_t i=0;i<4;i++){
        uint8_t row = (uint8_t)(first_visible + i);
        if(row >= ROW_TOTAL) break;
        int y = y0 + i*dy;

        if(row == s->cursor) canvas_draw_str(c, 2, y, ">");

        if(row < MODE_COUNT){
            /* Режимы */
            canvas_draw_str(c, 14, y, kModes[row].name);
            if(row == s->active){
                /* Галочка выровнена справа от текста, с запасом от скроллбара. */
                int check_x = (int)SCROLLBAR_X - TIMER_MARGIN - 10;
                if(check_x < 90) check_x = 90; /* не уезжаем слишком влево на коротких строках */
                draw_checkmark(c, check_x, y);
            }
        }else if(row == MODE_COUNT){
            /* Settings */
            canvas_draw_str(c, 14, y, "Settings");
        }else{
            /* Help */
            canvas_draw_str(c, 14, y, "Help");
        }
    }

    /* Пунктирный скроллбар: ползунок мгновенно следует за курсором. */
    draw_scrollbar_dotted(c, ROW_TOTAL, s->cursor);

    /* Всплывающая подсказка (внизу, белый текст на чёрной плашке по всей ширине). */
    if(s->hint_visible){
        const char* msg = "Long press back to exit";
        canvas_set_font(c, FontSecondary);
        uint16_t text_w = canvas_string_width(c, msg);
        uint16_t text_h = 10;
        uint16_t text_y = 64 - 2;
        canvas_set_color(c, ColorBlack);
        canvas_draw_box(c, 0, (uint16_t)(text_y - text_h), 128, (uint16_t)(text_h + 4));
        canvas_set_color(c, ColorWhite);
        uint16_t text_x = (uint16_t)((128 - text_w) / 2);
        canvas_draw_str(c, text_x, text_y, msg);
        canvas_set_color(c, ColorBlack);
    }
}

/* ===== UI: Help / Экран помощи (без заголовка) ===== */
static void draw_help(Canvas* c, const AppState* s){
    canvas_clear(c);
    canvas_set_font(c, FontSecondary);
    canvas_set_color(c, ColorBlack);

    uint8_t max_lines, max_top_line;
    help_layout_params(&max_lines, &max_top_line);

    const uint8_t top = 10;
    const uint8_t line_h = 9;
    for(uint8_t i=0;i<max_lines;i++){
        uint8_t idx = (uint8_t)(s->help_top_line + i);
        if(idx >= HELP_LINES_COUNT) break;
        canvas_draw_str(c, 2, (uint8_t)(top + i*line_h), HELP_LINES[idx]);
    }

    /* Скроллбар по верхней видимой строке: total_steps = max_top_line+1,
       чтобы самая нижняя позиция корректно достигалась. */
    uint16_t total_steps = (uint16_t)(max_top_line + 1);
    if(total_steps < 1) total_steps = 1;
    draw_scrollbar_dotted(c, total_steps, s->help_top_line);
}

/* ===== UI: Settings / Экран настроек ===== */
static void draw_settings(Canvas* c, const AppState* s){
    canvas_clear(c);

    /* Заголовок (слева) */
    canvas_set_font(c, FontPrimary);
    canvas_set_color(c, ColorBlack);
    canvas_draw_str(c, 4, 14, "Settings");

    /* Один параметр: "Limit run time" со значением Yes/No, курсор слева. */
    canvas_set_font(c, FontSecondary);
    const int y = 26; /* ближе к заголовку — удобнее визуально */
    canvas_draw_str(c, 2, y, ">");
    canvas_draw_str(c, 14, y, "Limit run time");

    const char* val = s->limit_runtime ? "Yes" : "No";
    uint16_t w = canvas_string_width(c, val);
    uint16_t right_x = (uint16_t)(SCROLLBAR_X - TIMER_MARGIN);
    uint16_t x = (w <= right_x) ? (uint16_t)(right_x - w) : 2;
    canvas_draw_str(c, x, y, val);

    /* Скроллбар ради единообразия — единственная позиция. */
    draw_scrollbar_dotted(c, 1, 0);
}

/* ===== Draw dispatch / Коммутатор отрисовки по экрану ===== */
static void draw_cb(Canvas* c, void* ctx){
    AppState* s = ctx;
    switch(s->screen){
        case ScreenMenu:      draw_menu(c, s); break;
        case ScreenHelp:      draw_help(c, s); break;
        case ScreenSettings:  draw_settings(c, s); break;
        default:              draw_menu(c, s); break;
    }
}

/* ===== Input plumbing (queue) / Прокладка ввода через очередь ===== */
typedef struct { FuriMessageQueue* q; } InputCtx;
static void vp_input_cb(InputEvent* e, void* ctx){
    InputCtx* ic = ctx;
    InputEvent ev = *e; /* копия, чтобы не держать чужую память */
    furi_message_queue_put(ic->q, &ev, 0);
}

/* ===== Main / Точка входа приложения ===== */
int32_t embraco_starter(void* p){
    UNUSED(p);

    /* Инициализируем всё состояние. По безопасности — стартуем в Help. */
    AppState s = {
        .screen = ScreenHelp,     // старт в Help
        .cursor = 0,
        .first_visible = 0,
        .active = 0,
        .help_top_line = 0,
        .limit_runtime = true,    // ограничение времени включено по умолчанию
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

    /* GUI + ViewPort + очередь ввода */
    s.gui = furi_record_open(RECORD_GUI);
    s.vp = view_port_alloc();
    s.q = furi_message_queue_alloc(8, sizeof(InputEvent));
    InputCtx ic = {.q = s.q};

    view_port_draw_callback_set(s.vp, draw_cb, &s);
    view_port_input_callback_set(s.vp, vp_input_cb, &ic);
    gui_add_view_port(s.gui, s.vp, GuiLayerFullscreen);

    /* Стартовая безопасность: пин в Hi‑Z, LED выкл. */
    pin_to_hiz();
    led_apply(&s, 0);

    /* Константы навигации: всего строк и вместимость окна. */
    const uint8_t ROW_TOTAL = MODE_COUNT + 2; // + Settings + Help
    const uint8_t MAX_ROWS = 4;

    bool exit = false;
    InputEvent ev;
    while(!exit){
        /* Обработка таймаута из one‑shot: делаем Power off в главном цикле. */
        if(s.timeout_expired){
            s.timeout_expired = false;
            apply_mode(&s, 0); // Power off
            s.cursor = 0;
            s.first_visible = 0;
            view_port_update(s.vp);
        }

        /* Чтение событий ввода (с таймаутом 100 мс). */
        if(furi_message_queue_get(s.q, &ev, 100) == FuriStatusOk){
            if(s.screen == ScreenHelp){
                /* Прокрутка списка Help + Back в меню. */
                if(ev.type == InputTypeShort || ev.type == InputTypeRepeat){
                    uint8_t max_lines, max_top_line;
                    help_layout_params(&max_lines, &max_top_line);

                    if(ev.key == InputKeyUp){
                        if(s.help_top_line>0) s.help_top_line--;
                    }else if(ev.key == InputKeyDown){
                        if(s.help_top_line < max_top_line) s.help_top_line++;
                    }else if(ev.key == InputKeyBack){
                        s.screen = ScreenMenu;
                        s.cursor = 0;
                        s.first_visible = 0;
                        apply_mode(&s, 0);
                    }
                }
            } else if(s.screen == ScreenSettings){
                /* Единственная настройка: Limit run time (Yes/No). */
                if(ev.type == InputTypeShort){
                    if(ev.key == InputKeyOk){
                        if(s.limit_runtime){
                            /* Смена Yes -> No требует подтверждения. */
                            bool confirm = show_limit_alert_confirm();
                            if(confirm){
                                s.limit_runtime = false; // Unlimited
                                stop_timers(&s);
                                s.remaining_ms = 0;
                            }
                        }else{
                            /* Смена No -> Yes — без предупреждений. */
                            s.limit_runtime = true;
                            if(s.active != 0) start_tick_timer_if_needed(&s);
                        }
                    }else if(ev.key == InputKeyBack){
                        s.screen = ScreenMenu;
                    }
                }
            } else { // ScreenMenu
                /* Навигация по главному меню (Up/Down/Ok/Back/Back long). */
                if(ev.type == InputTypeShort){
                    if(ev.key == InputKeyUp){
                        if(s.cursor == 0){
                            s.cursor = (uint8_t)(ROW_TOTAL - 1);
                            s.first_visible = (ROW_TOTAL > MAX_ROWS) ? (uint8_t)(ROW_TOTAL - MAX_ROWS) : 0;
                        }else{
                            s.cursor--;
                            if(s.cursor < s.first_visible) s.first_visible = s.cursor;
                        }
                    }else if(ev.key == InputKeyDown){
                        if(s.cursor == (uint8_t)(ROW_TOTAL - 1)){
                            s.cursor = 0;
                            s.first_visible = 0;
                        }else{
                            s.cursor++;
                            if(s.cursor >= s.first_visible + MAX_ROWS){
                                s.first_visible = (uint8_t)(s.cursor - (MAX_ROWS - 1));
                            }
                        }
                    }else if(ev.key == InputKeyOk){
                        if(s.cursor < MODE_COUNT){
                            /* Применяем выбранный режим. */
                            apply_mode(&s, s.cursor);
                        }else if(s.cursor == MODE_COUNT){
                            /* Заходим в Settings. */
                            s.screen = ScreenSettings;
                        }else{
                            /* Help: перед входом — стоп PWM и таймеры, Hi‑Z, LED off. */
                            pwm_hw_stop_safe(&s.pwm_running);
                            pin_to_hiz();
                            led_apply(&s, 0);
                            stop_timers(&s);
                            s.remaining_ms = 0;
                            s.timeout_expired = false;
                            s.screen = ScreenHelp;
                            s.help_top_line = 0;
                            s.active = 0;       // визуально — Power off
                            s.cursor = 0;
                            s.first_visible = 0;
                        }
                    }else if(ev.key == InputKeyBack){
                        /* Короткий Back — показываем подсказку про удержание для выхода. */
                        s.hint_visible = true;
                        if(!s.hint_timer){
                            s.hint_timer = furi_timer_alloc(hint_timer_cb, FuriTimerTypeOnce, &s);
                        }
                        furi_timer_start(s.hint_timer, furi_ms_to_ticks(1500));
                    }
                }else if(ev.type == InputTypeLong){
                    if(ev.key == InputKeyBack){
                        /* Долгое удержание Back — выход из приложения. */
                        exit = true;
                    }
                }
            }
            view_port_update(s.vp);
        }
    }

    /* ===== Cleanup / Финальная уборка ресурсов ===== */
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
