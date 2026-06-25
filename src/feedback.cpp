#include "feedback.h"

#ifndef UNIT_TEST
#include <M5Unified.h>

namespace {
    constexpr float    BEEP_HZ        = 2000.0f;
    constexpr uint32_t BEEP_MS        = 150;    
    constexpr uint8_t  BUZZER_VOLUME  = 128;    // S3's real ES8311 speaker is MUCH louder than a passive buzzer! 255 might blow it out or clip badly.
}

namespace feedback {

void begin() {
    // Completely removed the old GPIO 10 pin initialization to protect the S3 display bus.
    
    // M5.begin() already started the I2S Audio Codec.
    // We drop the volume scale slightly because the S3 speaker handles real power.
    M5.Speaker.setVolume(BUZZER_VOLUME);
}

void set_color(ColorState c) {
    // If you are outputting the RED/GREEN/BLUE sharpening states directly to the screen via ui.cpp,
    // you can safely leave this blank on the S3 to avoid GPIO conflicts.
    if (c == ColorState::RED) {
        // Optional: If using an external M5 Hex LED Hat or RGB unit, insert I2C/Wire commands here.
    }
}

void fault_led() {
    // Safe placeholder for S3 framework
}

void beep_out_of_tolerance() {
    if (!M5.Speaker.isEnabled()) return;
    M5.Speaker.tone(BEEP_HZ, BEEP_MS);
}

void beep_confirm() {
    if (!M5.Speaker.isEnabled()) return;
    // Your queuing logic is perfect for M5Unified. 
    // The second tone safely queues because stop_current_sound=false.
    M5.Speaker.tone(1500.0f, 90);
    M5.Speaker.tone(2200.0f, 120, -1, false);
}

} // namespace feedback

#else
// Native stubs
namespace feedback {
    void begin() {}
    void set_color(ColorState) {}
    void fault_led() {}
    void beep_out_of_tolerance() {}
    void beep_confirm() {}
}
#endif
