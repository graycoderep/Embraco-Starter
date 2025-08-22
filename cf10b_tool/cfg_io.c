    #include "cfg_io.h"
    #include "pins_table.h"
    #include <storage/storage.h>
    #include <furi.h>
    #include <stdio.h>
    #include <string.h>
    #include <stdlib.h>

    static const char* base_dir = "/apps_data/cf10b_tool/profiles";

    static const PinDef* find_pin_by_name(const char* s){
        size_t n; const PinDef* T = pins_get_table(&n);
        for(size_t i=0;i<n;i++) if(strcmp(T[i].name, s)==0) return &T[i];
        return NULL;
    }

    bool cfg_io_load_profile(const char* prof_name, GpioProfile* out){
        Storage* st = furi_record_open(RECORD_STORAGE);
        char path[128]; snprintf(path,sizeof(path), "%s/%s.cfg", base_dir, prof_name);
        File* f = storage_file_open(st, path, FSAM_READ, FSOM_OPEN_EXISTING);
        if(!f){ furi_record_close(RECORD_STORAGE); return false; }

        char line[128];
        while(storage_file_read_line(f, line, sizeof(line))){
            char k[64]={0}, v[64]={0};
            if(sscanf(line, "%63[^=]=%63s", k, v)==2){
                if(!strcmp(k,"out0")){
                    const PinDef* pd = find_pin_by_name(v); if(pd) out->out0 = pd->pin;
                } else if(!strcmp(k,"in0")){
                    const PinDef* pd = find_pin_by_name(v); if(pd) out->in0 = pd->pin;
                } else if(!strcmp(k,"active_high")) out->out_active_high = (strcmp(v,"0")!=0);
                else if(!strcmp(k,"debounce_ms")) out->debounce_ms = (uint16_t)atoi(v);
                else if(!strcmp(k,"pwm_freq_hz")) out->pwm_freq_hz = (uint16_t)atoi(v);
                else if(!strcmp(k,"pwm_duty_pc")) out->pwm_duty_pc = (uint8_t)atoi(v);
                else if(!strcmp(k,"boot_all_off")) out->safety.boot_all_off = (strcmp(v,"0")!=0);
                else if(!strcmp(k,"interlock_in0_blocks_out0")) out->safety.interlock_in0_blocks_out0 = (strcmp(v,"0")!=0);
            }
        }
        storage_file_close(f);
        furi_record_close(RECORD_STORAGE);
        return true;
    }

    bool cfg_io_save_profile(const char* prof_name, const GpioProfile* in){
        Storage* st = furi_record_open(RECORD_STORAGE);
        storage_common_mkdir(st, base_dir); // ensure dir
        char path[128]; snprintf(path,sizeof(path), "%s/%s.cfg", base_dir, prof_name);
        File* f = storage_file_open(st, path, FSAM_WRITE, FSOM_CREATE_ALWAYS);
        if(!f){ furi_record_close(RECORD_STORAGE); return false; }

        size_t n; const PinDef* T = pins_get_table(&n);
        const char* out0n = "NA"; const char* in0n = "NA";
        for(size_t i=0;i<n;i++){
            if(T[i].pin==in->out0) out0n = T[i].name;
            if(T[i].pin==in->in0)  in0n = T[i].name;
        }
        char buf[512];
        int len = snprintf(buf, sizeof(buf),
            "out0=%s
in0=%s
active_high=%d
debounce_ms=%u
pwm_freq_hz=%u
pwm_duty_pc=%u
"
            "boot_all_off=%d
interlock_in0_blocks_out0=%d
",
            out0n, in0n, in->out_active_high ? 1:0, in->debounce_ms, in->pwm_freq_hz,
            in->pwm_duty_pc, in->safety.boot_all_off ? 1:0, in->safety.interlock_in0_blocks_out0 ? 1:0);
        storage_file_write(f, buf, (uint16_t)len);
        storage_file_close(f);
        furi_record_close(RECORD_STORAGE);
        return true;
    }
