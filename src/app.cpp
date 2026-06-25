#include "app.h"
#include "angle.h"
#include "ui.h"
#include "feedback.h"
#include "settings.h"
#include "session.h"
#include <cmath>

namespace {
    // Only ever {0,0,0} (default) or a normalized result from CaptureFSM.
    static inline bool is_zero_vec(Vec3 v) {
        return v.x == 0.0f && v.y == 0.0f && v.z == 0.0f;
    }

    // Unit vector, or {0,0,0} for a degenerate (near-zero) input.
    static inline Vec3 normalized(Vec3 v) {
        float m = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
        if (m < 1e-3f) return {0.0f, 0.0f, 0.0f};
        return {v.x/m, v.y/m, v.z/m};
    }

    static PresetSelection next_preset(PresetSelection p) {
        switch (p) {
            case PresetSelection::P12:    return PresetSelection::P15;
            case PresetSelection::P15:    return PresetSelection::P17;
            case PresetSelection::P17:    return PresetSelection::P20;
            case PresetSelection::P20:    return PresetSelection::P22;
            case PresetSelection::P22:    return PresetSelection::CANCEL;
            case PresetSelection::CANCEL: return PresetSelection::P12;
        }
        __builtin_unreachable();
    }

    static inline Tolerance next_tolerance(Tolerance t) {
        switch (t) {
            case Tolerance::TIGHT:  return Tolerance::NORMAL;
            case Tolerance::NORMAL: return Tolerance::EASY;
            case Tolerance::EASY:   return Tolerance::TIGHT;
        }
        __builtin_unreachable();
    }
}

void App::begin(bool had_session_in_rtc_ram) {
    // Math filter time step initialized at 50 Hz.
    filter_.begin(50.0f, 0.8f, 0.02f);
    filter_.set_bias(settings::load_gyro_bias());
    buzzer_on_ = settings::load_buzzer();
    tol_       = settings::load_tolerance();

    // S3 Wake paths targeting the non-volatile session bounds.
    if (had_session_in_rtc_ram && session::has_session()) {
        const auto& s = session::state();
        target_deg_         = s.target_deg;
        tol_                = s.tolerance;
        g_flat_             = s.g_flat;
        edge_axis_          = s.edge_axis;
        strokes_a_          = s.strokes_A;
        strokes_b_          = s.strokes_B;
        session_started_ms_ = s.session_started_ms;
        side_fsm_.restore_side(s.current_side);
        
        if (is_zero_vec(g_flat_)) {
            transition(State::ZERO_CAL, 0);
            zc_substate_ = ZeroCalSubstate::PROMPT_FLAT;
            return;
        }
        transition(State::RESUME_PROMPT, 0);
        return;
    }
    transition(State::BOOT, 0);
}

void App::transition(State to, uint32_t now_ms) {
    state_            = to;
    state_entered_ms_ = now_ms;
    last_activity_ms_ = now_ms;
    last_stroke_ms_   = now_ms;

    switch (to) {
        case State::BOOT:          ui::draw_boot(); break;
        case State::SET_TARGET:
            in_preset_mode_   = false;
            preset_selection_ = PresetSelection::P12;
            ui::draw_set_target(target_deg_, in_preset_mode_, preset_selection_);
            break;
        case State::SET_TOLERANCE: ui::draw_set_tolerance(tol_); break;
        case State::ZERO_CAL:
            ui::clear();
            zc_rendered_ = ZeroCalSubstate::DONE;
            break;
        case State::REZERO:
            ui::clear();
            zc_fsm_.start();
            break;
        case State::ACTIVE: {
            stroke_fsm_.reset();
            if (session_started_ms_ == 0) session_started_ms_ = now_ms;
            save_session_();
            break;
        }
        case State::SUMMARY: {
            uint32_t dur_s = (session_started_ms_ != 0 && now_ms >= session_started_ms_)
                             ? (now_ms - session_started_ms_) / 1000 : 0;
            ui::draw_summary(target_deg_, tol_, strokes_a_, strokes_b_, dur_s);
            break;
        }
        case State::FAULT:   ui::draw_fault(fault_code_); feedback::fault_led(); break;
        case State::RESUME_PROMPT:
            last_countdown_sec_ = 5;
            ui::draw_resume_prompt(target_deg_, tol_, strokes_a_, strokes_b_, 5);
            break;
        case State::SLEEP:   break;
    }
}

void App::refresh_gyro_bias_(Vec3 bias) {
    filter_.set_bias(bias);
    settings::save_gyro_bias(bias);
}

void App::save_session_() {
    SessionState ss;
    ss.target_deg         = target_deg_;
    ss.tolerance          = tol_;
    ss.g_flat             = g_flat_;
    ss.edge_axis          = edge_axis_;
    ss.strokes_A          = strokes_a_;
    ss.strokes_B          = strokes_b_;
    ss.current_side       = side_fsm_.current_side();
    ss.session_started_ms = session_started_ms_;
    session::mark_active(ss);
}

void App::on_tick(const Tick& t) {
    if (t.imu_fault != FaultCode::NONE) {
        fault_code_ = t.imu_fault;
        transition(State::FAULT, t.now_ms);
        return;
    }

    // Wake timer countdown logic
    if (state_ == State::RESUME_PROMPT) {
        if (t.input == InputEvent::B_SHORT) {
            transition(State::ACTIVE, t.now_ms);
            return;
        }
        if (t.input == InputEvent::A_SHORT) {
            strokes_a_ = strokes_b_ = 0;
            session_started_ms_ = 0;
            session::clear();
            transition(State::SET_TARGET, t.now_ms);
            return;
        }
        int elapsed_s = (t.now_ms - state_entered_ms_) / 1000;
        int remaining_s = 5 - elapsed_s;
        if (remaining_s <= 0) {
            transition(State::ACTIVE, t.now_ms);
            return;
        }
        if (remaining_s != last_countdown_sec_) {
            ui::draw_resume_prompt(target_deg_, tol_, strokes_a_, strokes_b_, remaining_s);
            last_countdown_sec_ = remaining_s;
        }
        return;
    }

    switch (state_) {
        case State::BOOT:          handle_boot(t);          break;
        case State::SET_TARGET:     handle_set_target(t);     break;
        case State::SET_TOLERANCE:  handle_set_tolerance(t);  break;
        case State::ZERO_CAL:       handle_zero_cal(t);       break;
        case State::ACTIVE:         handle_active(t);         break;
        case State::REZERO:         handle_rezero(t);         break;
        case State::SUMMARY:        handle_summary(t);        break;
        case State::FAULT:                                    break;
        case State::RESUME_PROMPT:                            break;
        case State::SLEEP:                                    break;
    }
}

void App::handle_boot(const Tick& t) {
    if (t.now_ms - state_entered_ms_ >= 2000) {
        transition(State::SET_TARGET, t.now_ms);
    }
}

void App::handle_zero_cal(const Tick& t) {
    InputEvent input = t.input;
    if (input == InputEvent::A_LONG) {
        transition(State::SET_TARGET, t.now_ms);
        return;
    }

    switch (zc_substate_) {
        case ZeroCalSubstate::PROMPT_FLAT:
            if (input == InputEvent::A_SHORT) {
                zc_fsm_.start();
                zc_substate_ = ZeroCalSubstate::CAPTURE_FLAT;
            }
            break;
        case ZeroCalSubstate::CAPTURE_FLAT: {
            bool done = false;
            if (input == InputEvent::B_SHORT) {
                Vec3 forced = normalized(t.accel_g);
                if (!is_zero_vec(forced)) { g_flat_ = forced; done = true; }
            } else {
                zc_fsm_.update(t.accel_g, t.gyro_dps);
                if (zc_fsm_.done()) {
                    g_flat_ = zc_fsm_.result();
                    refresh_gyro_bias_(zc_fsm_.gyro_bias());
                    done = true;
                }
            }
            if (done) zc_substate_ = ZeroCalSubstate::PROMPT_RAISE;
            break;
        }
        case ZeroCalSubstate::PROMPT_RAISE:
            if (input == InputEvent::A_SHORT) {
                zc_fsm_.start();
                zc_substate_ = ZeroCalSubstate::CAPTURE_RAISE;
            }
            break;
        case ZeroCalSubstate::CAPTURE_RAISE: {
            bool done = false;
            Vec3 raised = {0.0f, 0.0f, 0.0f};
            if (input == InputEvent::B_SHORT) {
                raised = normalized(t.accel_g);
                if (!is_zero_vec(raised)) done = true;
            } else {
                zc_fsm_.update(t.accel_g, t.gyro_dps);
                if (zc_fsm_.done()) { raised = zc_fsm_.result(); done = true; }
            }
            if (done) {
                edge_axis_ = compute_edge_axis(g_flat_, raised);
                zc_substate_ = ZeroCalSubstate::DONE;
                session_started_ms_ = t.now_ms;
                side_fsm_.reset();   
                transition(State::ACTIVE, t.now_ms);
            }
            break;
        }
        case ZeroCalSubstate::DONE: break;
    }

    int ticks_remaining = zc_fsm_.warmup_remaining() + zc_fsm_.averaging_remaining();
    if (zc_fsm_.phase() == zero_cal::Phase::WARMUP) {
        ticks_remaining += zero_cal::AVERAGING_TICKS;
    }
    int total_capture_ms_remaining = ticks_remaining * (int)kLoopTickMs;
    bool moving = zc_fsm_.moving();

    zero_cal::Phase ph = zc_fsm_.phase();
    if (input == InputEvent::A_SHORT || input == InputEvent::B_SHORT || moving ||
        ph == zero_cal::Phase::WARMUP || ph == zero_cal::Phase::AVERAGING) {
        last_activity_ms_ = t.now_ms;
    }

    bool retry = false;
    switch (zc_substate_) {
        case ZeroCalSubstate::PROMPT_FLAT:
            if (zc_rendered_ != zc_substate_) ui::draw_zero_cal_prompt(1, retry);
            break;
        case ZeroCalSubstate::CAPTURE_FLAT:  ui::draw_zero_cal_progress(total_capture_ms_remaining, moving); break;
        case ZeroCalSubstate::PROMPT_RAISE:
            if (zc_rendered_ != zc_substate_) ui::draw_zero_cal_prompt(2, retry);
            break;
        case ZeroCalSubstate::CAPTURE_RAISE: ui::draw_zero_cal_progress(total_capture_ms_remaining, moving); break;
        case ZeroCalSubstate::DONE:          break;
    }
    zc_rendered_ = zc_substate_;
}

void App::handle_set_target(const Tick& t) {
    if (t.input == InputEvent::A_SHORT) {
		// NOTE: This leading block assumes it is the inner body of "void App::handle_set_target(const Tick& t)"
		if (t.input == InputEvent::A_SHORT)
		{
			if (in_preset_mode_)
			{
				if (preset_selection_ == PresetSelection::CANCEL)
				{
					in_preset_mode_ = false;
					last_activity_ms_ = t.now_ms;
				}
				else
				{
					target_deg_ = preset_degrees(preset_selection_);
					transition(State::SET_TOLERANCE, t.now_ms);
				}
			}
			else
			{
				transition(State::SET_TOLERANCE, t.now_ms);
			}
		}
		else if (t.input == InputEvent::B_SHORT)
		{
			if (!in_preset_mode_)
			{
				in_preset_mode_   = true;
				preset_selection_ = PresetSelection::P12;
			}
			else
			{
				preset_selection_ = next_preset(preset_selection_);
			}
			last_activity_ms_ = t.now_ms;
		}

		if (state_ == State::SET_TARGET && t.input != InputEvent::NONE)
		{
			ui::draw_set_target(target_deg_, in_preset_mode_, preset_selection_);
		}
	}
}

void App::handle_set_tolerance(const Tick& t)
{
    if (t.input == InputEvent::B_SHORT)
    {
        tol_ = next_tolerance(tol_);
        last_activity_ms_ = t.now_ms;
        ui::draw_set_tolerance(tol_);
    }
    else if (t.input == InputEvent::A_SHORT)
    {
        settings::save_tolerance(tol_);
        zc_substate_ = ZeroCalSubstate::PROMPT_FLAT;
        transition(State::ZERO_CAL, t.now_ms);
    }
}

void App::handle_active(const Tick& t)
{
    filter_.update(t.gyro_dps, t.accel_g);

    if (snap_cooldown_ > 0)
    {
        --snap_cooldown_;
    }
    else if (mahony::should_snap(filter_.gravity(), t.accel_g, t.gyro_dps))
    {
        filter_.nudge_to_gravity(t.accel_g);
        snap_cooldown_ = mahony::SNAP_COOLDOWN_TICKS;
    }

    Vec3 g_now = filter_.gravity();
    float bevel = bevel_angle(g_flat_, edge_axis_, g_now);
    ColorState col = classify(bevel, target_deg_, tolerance_degrees(tol_));

    // Linear translation stroke extraction
    Vec3 la = { t.accel_g.x - g_now.x, t.accel_g.y - g_now.y, t.accel_g.z - g_now.z };
    float la_v = la.x * g_now.x + la.y * g_now.y + la.z * g_now.z;
    Vec3 la_h = { la.x - la_v * g_now.x, la.y - la_v * g_now.y, la.z - la_v * g_now.z };
    float lat = std::sqrt(la_h.x * la_h.x + la_h.y * la_h.y + la_h.z * la_h.z);

    if (lat >= StrokeFSM::PEAK_LOW_G) 
    {
        last_activity_ms_ = t.now_ms;
    }

    bool in_tol = (col == ColorState::GREEN);
    uint32_t before = stroke_fsm_.stroke_count();
    stroke_fsm_.update(t.now_ms, in_tol, lat);

    if (stroke_fsm_.stroke_count() > before)
    {
        if (side_fsm_.current_side() == Side::A) strokes_a_++;
        else                                      strokes_b_++;
        last_stroke_ms_ = t.now_ms;
        save_session_();
    }

    if (t.input == InputEvent::A_LONG)
    {
        transition(State::SUMMARY, t.now_ms);
        return;
    }
    if (t.input == InputEvent::A_SHORT)
    {
        transition(State::REZERO, t.now_ms);
        return;
    }
    if (t.input == InputEvent::B_SHORT)
    {
        side_fsm_.manual_toggle(t.now_ms);
        side_fsm_.consume_switch();
        stroke_fsm_.reset();
        last_activity_ms_ = t.now_ms;
        save_session_();
    }

    if (t.input == InputEvent::B_LONG)
    {
        buzzer_on_ = !buzzer_on_;
        settings::save_buzzer(buzzer_on_);
        if (buzzer_on_) feedback::beep_confirm();
        buzzer_flash_until_   = t.now_ms + 800;
        buzzer_flash_showing_ = true;
        last_activity_ms_     = t.now_ms;
    }
    else if (buzzer_flash_showing_ && t.now_ms > buzzer_flash_until_)
    {
        buzzer_flash_showing_ = false;
    }

    feedback::set_color(col);

    if (buzzer_on_ && col != ColorState::GREEN && prev_color_ == ColorState::GREEN)
    {
        feedback::beep_out_of_tolerance();
    }
    prev_color_ = col;

    ui::ActiveView v{ col, side_fsm_.current_side(), strokes_a_, strokes_b_,
                      buzzer_flash_showing_, buzzer_on_, bevel };
    ui::draw_active(v);
}

void App::handle_rezero(const Tick& t)
{
    if (t.input == InputEvent::B_SHORT || t.input == InputEvent::A_LONG)
    {
        ui::clear();
        transition(State::ACTIVE, t.now_ms);
        return;
    }

    zc_fsm_.update(t.accel_g, t.gyro_dps);
    if (zc_fsm_.done())
    {
        g_flat_ = zc_fsm_.result();
        refresh_gyro_bias_(zc_fsm_.gyro_bias());
        ui::clear();
        transition(State::ACTIVE, t.now_ms);
        return;
    }

    int ticks_remaining = zc_fsm_.warmup_remaining() + zc_fsm_.averaging_remaining();
    if (zc_fsm_.phase() == zero_cal::Phase::WARMUP) 
    {
        ticks_remaining += zero_cal::AVERAGING_TICKS;
    }
    ui::draw_zero_cal_progress(ticks_remaining * (int)kLoopTickMs, zc_fsm_.moving());

    zero_cal::Phase ph = zc_fsm_.phase();
    if (zc_fsm_.moving() || ph == zero_cal::Phase::WARMUP || ph == zero_cal::Phase::AVERAGING)
    {
        last_activity_ms_ = t.now_ms;
    }
}

void App::handle_summary(const Tick& t)
{
    if (t.input == InputEvent::A_SHORT)
    {
        strokes_a_ = strokes_b_ = 0;
        session_started_ms_ = 0;
        session::clear();
        transition(State::SET_TARGET, t.now_ms);
    }
    else if (t.input == InputEvent::B_SHORT)
    {
        session::clear();
        strokes_a_ = strokes_b_ = 0;
        session_started_ms_ = 0;
        transition(State::SLEEP, t.now_ms);
    }
}
