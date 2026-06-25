#include "power.h"
#include "ui.h"

#ifndef UNIT_TEST
#include <M5Unified.h>
#include <esp_sleep.h>
#endif

namespace power {

IdleConfig config_for(State s) {
    switch (s) {
        case State::BOOT:
        case State::RESUME_PROMPT:
        case State::SLEEP:
            return {0, 0};
        case State::ZERO_CAL:       return { 60000, 120000};
        case State::REZERO:         return { 60000, 120000};
        case State::FAULT:          return { 60000, 300000};
        case State::SET_TARGET:     return { 90000, 120000};
        case State::SET_TOLERANCE:  return { 60000,  90000};
        case State::ACTIVE:         return {180000, 300000}; 
        case State::SUMMARY:        return { 60000,  90000};
    }
    __builtin_unreachable();
}

void begin() {}

bool check_idle(uint32_t now_ms, State current,
                uint32_t last_activity_ms, uint32_t last_stroke_ms)
{
    auto cfg = config_for(current);
    if (cfg.sleep_ms == 0) return false;

    uint32_t reference = (current == State::ACTIVE)
        ? (last_stroke_ms > last_activity_ms ? last_stroke_ms : last_activity_ms)
        : last_activity_ms;
    return (now_ms - reference) >= cfg.sleep_ms;
}

void update_backlight(uint32_t now_ms, State current,
                      uint32_t last_activity_ms, uint32_t last_stroke_ms)
{
    auto cfg = config_for(current);
    uint8_t baseline_pct = (current == State::ACTIVE) ? 80 : 100;
    uint8_t dim_pct = (current == State::ACTIVE) ? 30 : 15;

    if (cfg.dim_ms == 0) {
        ui::set_backlight(baseline_pct);
        return;
    }
    uint32_t reference = (current == State::ACTIVE)
        ? (last_stroke_ms > last_activity_ms ? last_stroke_ms : last_activity_ms)
        : last_activity_ms;
    uint32_t idle = now_ms - reference;
    uint8_t pct = (idle >= cfg.dim_ms) ? dim_pct : baseline_pct;
    ui::set_backlight(pct);
}

#ifndef UNIT_TEST
[[noreturn]] void enter_deep_sleep() {
    // Blank the display cleanly through the M5Unified wrapper
    M5.Display.setBrightness(0);
    M5.Display.sleep();

    // M5Unified automatically manages the native power configurations on the S3.
    // It configures BtnPWR / GPIO 41 internally as an active-low wake source.
    // Calling deepSleep() here handles the proper power-down sequencing.
    M5.Power.deepSleep();  
    
    while (true) {}  // deepSleep() does not return
}
#else
[[noreturn]] void enter_deep_sleep() {
    while (true) {}
}
#endif

} // namespace power
