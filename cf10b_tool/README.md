# CF10B Tool — Flipper Zero GPIO App (multi-model, runtime pin editing)

This app lets you:
- Select **Model** = {FREQUENCY, SERIAL, DROPIN} at runtime
- **Edit pin mapping** for each model from the UI (OUT/IN, polarity, debounce, PWM/frequency)
- Persist per-model settings to SD card at `/apps_data/cf10b_tool/profiles/<Model>.cfg`
- Safe boot: outputs stay **OFF** until a valid profile is active

## Folder layout (drop under Flipper SDK)
Place this folder under your Flipper SDK: `applications_user/cf10b_tool/`

```
applications_user/cf10b_tool/
  application.fam
  gpio_tool_app.c
  profile.h
  profile.c
  pins_table.h
  pins_table.c      <-- EDIT THIS with your SDK's exported pin symbols
  cfg_io.h
  cfg_io.c
  engine.h
  engine.c
  ui_helpers.h
  ui_helpers.c
  README.md
```

## Build (uFBT / FBT)
1) Clone Flipper Zero firmware SDK and set up toolchain (per official docs).
2) Copy this folder to `applications_user/cf10b_tool/` in the SDK tree.
3) Build user apps only (faster):
   ```bash
   ./fbt COMPACT=1 fap_configure fap_build
   ```
   or full firmware:
   ```bash
   ./fbt firmware
   ```
4) Flash or install the resulting `.fap`. The app appears as **CF10B Tool**.

## IMPORTANT: Pin Symbols
- Open `pins_table.c` and replace the sample `extern const GpioPin ...` declarations
  with **actual exported pins** from your SDK headers (e.g., `gpio_ext_pc0`, `gpio_ext_pc1`, etc.).
- Only list **safe, external, 3.3 V** pins.

## Models
- **FREQUENCY**: Outputs a square wave at 50% duty. UI controls RPM; app converts to Hz (RPM = 30*Hz),
  clamps freq to 66..150 Hz, caps RPM to 4500. Use an **isolated driver** (opto/digital isolator)
  so CF10B sees >=4.5 V logic on its frequency input.
- **SERIAL**: Prepares CF10B 600-baud frames (A5 CMD LSB MSB CK). We provide the frame builder; you must
  connect to a **600 baud, isolated UART**. Fill `engine_serial_send_bytes()` with your SDK UART TX routine
  or use an opto-isolated bit-bang (advanced).
- **DROPIN**: Read-only placeholders for AC sense are included; actual mains interface requires an isolated
  AC sense or SSR. We keep outputs OFF unless explicitly armed.

## Safety
- Outputs start OFF; profiles must be valid before arming outputs.
- Interlocks are enforced every loop (e.g., "IN blocks OUT").
- 3.3 V logic only. Use proper isolation/level shifting for CF10B.

## SD Overrides
- Files live at `/apps_data/cf10b_tool/profiles/<Model>.cfg`
- Example file content:
  ```ini
  out0=PC0
  in0=PC1
  active_high=1
  debounce_ms=20
  pwm_freq_hz=150
  pwm_duty_pc=50
  boot_all_off=1
  interlock_in0_blocks_out0=1
  ```
