#ifndef UNIT_TEST
#include <Arduino.h>
#include <M5Unified.h>
#include <esp_sleep.h>

#include "app.h"
#include "imu.h"
#include "input.h"
#include "settings.h"
#include "session.h"
#include "ui.h"
#include "feedback.h"
#include "power.h"

static App       g_app;
static InputFSM  g_input;

static uint32_t  g_next_tick_ms       = 0;
static constexpr uint32_t TICK_PERIOD_MS = kLoopTickMs; // 50 Hz — ample for human motion (1-5 Hz)

// A single imu::read() returning false simply means the BMI270 data interrupt 
// packet hasn't hit the internal registries yet. Only a sustained run of 
// failures (0.5 s) indicates a genuinely unresponsive IMU bus.
static constexpr uint32_t IMU_FAULT_TICKS = 25;

void setup() {
    auto cfg = M5.config();
    cfg.internal_imu = true;
    cfg.internal_spk = true;
    M5.begin(cfg);
    
    // Core S3 Frequency: Kept at 240 MHz to preserve stable I2S timing requirements 
    // for the ES8311 audio speaker driver codec.
    setCpuFrequencyMhz(240);  

    // Removed legacy AXP192 getKeyState() which causes core panics on S3 hardware.

    ui::begin();
    feedback::begin();
    power::begin();
    settings::begin();
    session::begin();

    // Standardized M5Unified wake detection: checks if the S3 booted from a 
    // deep-sleep wake cycle triggered by the native Power Button or timers.
	esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
	bool had_session_in_rtc = (cause != ESP_SLEEP_WAKEUP_UNDEFINED) && session::has_session();

    FaultCode fc = imu::begin();

    g_app.begin(had_session_in_rtc);

    // Push initial fault if IMU initialization failed.
    if (fc != FaultCode::NONE) {
        App::Tick t{millis(), InputEvent::NONE, {0,0,-1}, {0,0,0}, fc};
        g_app.on_tick(t);
    }

    g_next_tick_ms = millis();
}

void loop() {
    uint32_t now = millis();
    if ((int32_t)(now - g_next_tick_ms) < 0) {
        delay(1);
        return;
    }
    
    // Catch-up clamp: if the scheduler fell more than 5 periods behind, 
    // resync instead of replaying ticks back-to-back to prevent over-integrating 
    // the Mahony filter vector.
    if ((int32_t)(now - g_next_tick_ms) > (int32_t)(5 * TICK_PERIOD_MS)) {
        g_next_tick_ms = now;
    }
    g_next_tick_ms += TICK_PERIOD_MS;

    M5.update();
    
    // Power-key short press = "off" (triggers safe deep sleep closure).
    // M5Unified automatically binds BtnPWR to GPIO 41 on the M5StickS3.
    if (M5.BtnPWR.wasClicked()) {
        power::enter_deep_sleep();
    }
    
    // Native S3 physical button mapping arrays
    bool a_pressed = M5.BtnA.isPressed();
    bool b_pressed = M5.BtnB.isPressed();
    InputEvent ev = g_input.update(now, a_pressed, b_pressed);

    // Read the BMI270 sensor. Transient misses are normal; trigger E02 if 
    // consecutive failures surpass the half-second threshold.
    static uint32_t imu_read_fails = 0;
    Vec3 accel = {0,0,-1}, gyro = {0,0,0};
    FaultCode fault = FaultCode::NONE;
    if (!imu::read(accel, gyro)) {
        if (++imu_read_fails >= IMU_FAULT_TICKS) fault = FaultCode::E02_SELF_TEST_FAILED;
    } else {
        imu_read_fails = 0;
    }

    App::Tick tick{now, ev, accel, gyro, fault};
    g_app.on_tick(tick);

    // Sleep check using App's real last-activity / last-stroke timestamps.
    if (power::check_idle(now, g_app.current(),
                          g_app.last_activity_ms(),
                          g_app.last_stroke_ms())) {
        power::enter_deep_sleep();
    }
    power::update_backlight(now, g_app.current(),
                            g_app.last_activity_ms(),
                            g_app.last_stroke_ms());

    // If state entered SLEEP explicitly, execute sleep operations.
    if (g_app.current() == State::SLEEP) {
        power::enter_deep_sleep();
    }
}
#endif
