#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/variable_item_list.h>
#include <input/input.h>
#include <stdlib.h>
#include <stdio.h>
#include "profile.h"
#include "pins_table.h"
#include "cfg_io.h"
#include "engine.h"
#include "ui_helpers.h"

static View* g_status = NULL;
static EngineState g_engine;

// ===== STATUS VIEW =====
static void status_draw(Canvas* c, void* ctx){
    (void)ctx;
    canvas_clear(c);
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 2, 14, "CF10B Tool");

    canvas_set_font(c, FontSecondary);
    char ln[64];
    snprintf(ln,sizeof(ln),"Model: %s %s", g_rt.active->name, g_rt.locked?"(LOCKED)":"");
    canvas_draw_str(c, 2, 30, ln);

    // Show pin names
    snprintf(ln,sizeof(ln),"OUT0: %s", pins_name_from_ptr(g_rt.active->out0));
    canvas_draw_str(c, 2, 44, ln);

    // Show model-specific info
    if(g_rt.active->kind == MODEL_FREQUENCY){
        uint16_t hz = g_engine.current_freq_hz;
        uint16_t rpm = hz * 30;
        snprintf(ln,sizeof(ln),"Freq: %u Hz  RPM: %u", hz, rpm);
        canvas_draw_str(c, 2, 58, ln);
    } else if(g_rt.active->kind == MODEL_SERIAL){
        canvas_draw_str(c, 2, 58, "Serial: 600 baud frame builder");
    } else {
        canvas_draw_str(c, 2, 58, "Drop-In: monitor");
    }
}

static bool status_input(InputEvent* e, void* ctx){
    (void)ctx;
    if(e->type != InputTypeShort) return false;
    if(e->key==InputKeyOk){ ui_go_view(VIEW_MODEL_MENU); return true; }
    if(e->key==InputKeyUp){ ui_go_view(VIEW_EDIT_MODEL); return true; }
    return false;
}

// ===== MODEL MENU =====
static void on_model_select(void* ctx, uint32_t idx){
    (void)ctx;
    if(idx < MODEL_COUNT){
        if(!g_rt.locked){
            profile_set_active((uint8_t)idx);
            engine_apply_profile(&g_engine, g_rt.active);
        }
    } else if(idx == 999){
        profile_lock(!g_rt.locked);
    }
    ui_go_view(VIEW_STATUS);
}

static void build_model_menu(void){
    submenu_reset(g_submenu);
    submenu_set_header(g_submenu, "Select Model / Lock");
    for(uint8_t i=0;i<profile_count();++i){
        GpioProfile* p = profile_at(i);
        submenu_add_item(g_submenu, p->name, i, on_model_select, NULL);
    }
    submenu_add_item(g_submenu, g_rt.locked ? "Unlock Profile" : "Lock Profile", 999, on_model_select, NULL);
}

// ===== EDITOR (VariableItemList) =====
typedef struct { size_t pin_table_len; const PinDef* T; int out_idx; int in_idx; } EditCtx;
static EditCtx ec;

static void editor_commit(void* ctx){
    (void)ctx;
    if(!g_rt.locked){
        if(ec.out_idx>=0 && ec.out_idx<(int)ec.pin_table_len) g_rt.active->out0 = ec.T[ec.out_idx].pin;
        if(ec.in_idx>=0  && ec.in_idx<(int)ec.pin_table_len) g_rt.active->in0  = ec.T[ec.in_idx].pin;
        profile_save_override(g_rt.active);
        engine_apply_profile(&g_engine, g_rt.active);
    }
    ui_go_view(VIEW_STATUS);
}

static size_t find_pin_index(const GpioPin* p, const PinDef* T, size_t n){
    for(size_t i=0;i<n;i++) if(T[i].pin==p) return i;
    return 0;
}

// Callbacks must be standalone functions in C
static void cb_out0_change(VariableItem* item, void* ctx, uint8_t idx){
    (void)ctx; ec.out_idx = idx;
    variable_item_set_current_value_text(item, ec.T[idx].name);
}
static void cb_in0_change(VariableItem* item, void* ctx, uint8_t idx){
    (void)ctx; ec.in_idx = idx;
    variable_item_set_current_value_text(item, ec.T[idx].name);
}
static void cb_active_high_change(VariableItem* item, void* ctx, uint8_t idx){
    (void)ctx; bool v = (idx==1); g_rt.active->out_active_high = v;
    variable_item_set_current_value_text(item, v ? "Yes" : "No");
}
static void cb_debounce_change(VariableItem* item, void* ctx, uint8_t idx){
    (void)ctx; static const uint16_t deb_steps[] = {5,10,20,30,50,100};
    g_rt.active->debounce_ms = deb_steps[idx];
    char b[8]; snprintf(b,sizeof(b), "%u", deb_steps[idx]);
    variable_item_set_current_value_text(item, b);
}
static void cb_freq_change(VariableItem* item, void* ctx, uint8_t idx){
    (void)ctx; static const uint16_t f_steps[] = {66,100,120,130,140,150};
    g_rt.active->pwm_freq_hz = f_steps[idx];
    char b[8]; snprintf(b,sizeof(b), "%u", f_steps[idx]);
    variable_item_set_current_value_text(item, b);
}
static void cb_duty_change(VariableItem* item, void* ctx, uint8_t idx){
    (void)ctx; g_rt.active->pwm_duty_pc = idx*10;
    char b[8]; snprintf(b,sizeof(b), "%u", idx*10);
    variable_item_set_current_value_text(item, b);
}

static void build_editor(void){
    variable_item_list_reset(g_vil);
    size_t n; ec.T = pins_get_table(&n); ec.pin_table_len = n;

    ec.out_idx = (int)find_pin_index(g_rt.active->out0, ec.T, n);
    ec.in_idx  = (int)find_pin_index(g_rt.active->in0,  ec.T, n);

    VariableItem* it = variable_item_list_add(g_vil, "OUT0 Pin", n, cb_out0_change, NULL);
    variable_item_set_current_value_index(it, ec.out_idx);
    variable_item_set_current_value_text(it, ec.T[ec.out_idx].name);

    it = variable_item_list_add(g_vil, "IN0 Pin", n, cb_in0_change, NULL);
    variable_item_set_current_value_index(it, ec.in_idx);
    variable_item_set_current_value_text(it, ec.T[ec.in_idx].name);

    it = variable_item_list_add(g_vil, "OUT Active-High", 2, cb_active_high_change, NULL);
    variable_item_set_current_value_index(it, g_rt.active->out_active_high ? 1:0);
    variable_item_set_current_value_text(it, g_rt.active->out_active_high ? "Yes" : "No");

    it = variable_item_list_add(g_vil, "Debounce (ms)", 6, cb_debounce_change, NULL);
    static const uint16_t deb_steps[] = {5,10,20,30,50,100};
    uint8_t d_idx=2; for(uint8_t i=0;i<6;i++) if(deb_steps[i]==g_rt.active->debounce_ms) d_idx=i;
    variable_item_set_current_value_index(it, d_idx);
    char buf[8]; snprintf(buf,sizeof(buf), "%u", deb_steps[d_idx]);
    variable_item_set_current_value_text(it, buf);

    if(g_rt.active->kind == MODEL_FREQUENCY){
        it = variable_item_list_add(g_vil, "Frequency (Hz)", 6, cb_freq_change, NULL);
        static const uint16_t f_steps[] = {66,100,120,130,140,150};
        uint8_t f_idx=0; for(uint8_t i=0;i<6;i++) if(f_steps[i]==g_rt.active->pwm_freq_hz) f_idx=i;
        variable_item_set_current_value_index(it, f_idx);
        snprintf(buf,sizeof(buf), "%u", f_steps[f_idx]);
        variable_item_set_current_value_text(it, buf);

        it = variable_item_list_add(g_vil, "PWM Duty (%)", 11, cb_duty_change, NULL);
        uint8_t d = g_rt.active->pwm_duty_pc/10;
        variable_item_set_current_value_index(it, d);
        snprintf(buf,sizeof(buf), "%u", d*10);
        variable_item_set_current_value_text(it, buf);
    }

    variable_item_list_set_enter_callback(g_vil, editor_commit, NULL);
}

// ===== ENTRY POINT =====
int32_t cf10b_tool_app(void* p){
    UNUSED(p);

    profile_init();
    engine_init(&g_engine, g_rt.active);

    g_vd = view_dispatcher_alloc();
    Gui* gui = furi_record_open(RECORD_GUI);
    view_dispatcher_attach_to_gui(g_vd, gui, ViewDispatcherTypeFullscreen);

    // STATUS
    g_status = view_alloc();
    view_set_draw_callback(g_status, status_draw);
    view_set_input_callback(g_status, status_input);
    view_dispatcher_add_view(g_vd, VIEW_STATUS, g_status);

    // MODEL MENU
    g_submenu = submenu_alloc();
    build_model_menu();
    view_dispatcher_add_view(g_vd, VIEW_MODEL_MENU, submenu_get_view(g_submenu));

    // EDITOR
    g_vil = variable_item_list_alloc();
    build_editor();
    view_dispatcher_add_view(g_vd, VIEW_EDIT_MODEL, variable_item_list_get_view(g_vil));

    // Start
    ui_go_view(VIEW_STATUS);

    // Main loop
    while(1){
        engine_tick(&g_engine, g_rt.active);
        // redraw ~20 ms
        view_port_update(view_dispatcher_get_current_view_port(g_vd));
        furi_delay_ms(20);
    }

    // Cleanup (not reached normally)
    variable_item_list_free(g_vil);
    submenu_free(g_submenu);
    view_free(g_status);
    furi_record_close(RECORD_GUI);
    view_dispatcher_free(g_vd);
    return 0;
}
