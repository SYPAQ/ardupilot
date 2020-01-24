#include "Plane.h"
#include <AP_Common/AP_FWVersion.h>

/*****************************************************************************
*   The init_ardupilot function processes everything we need for an in - air restart
*        We will determine later if we are actually on the ground and process a
*        ground start in that case.
*
*****************************************************************************/

static void mavlink_delay_cb_static()
{
    plane.mavlink_delay_cb();
}

static void failsafe_check_static()
{
    plane.failsafe_check();
}

void Plane::init_ardupilot()
{
    // initialise serial port
    serial_manager.init_console();

    hal.console->printf("\n\nInit %s"
                        "\n\nFree RAM: %u\n",
                        AP::fwversion().fw_string,
                        (unsigned)hal.util->available_memory());

    //
    // Check the EEPROM format version before loading any parameters from EEPROM
    //
    load_parameters();

#if STATS_ENABLED == ENABLED
    // initialise stats module
    g2.stats.init();
#endif

#if HIL_SUPPORT
    if (g.hil_mode == 1) {
        // set sensors to HIL mode
        ins.set_hil_mode();
        compass.set_hil_mode();
        barometer.set_hil_mode();
    }
#endif

    ins.set_log_raw_bit(MASK_LOG_IMU_RAW);

    set_control_channels();

    mavlink_system.sysid = g.sysid_this_mav;

    // initialise serial ports
    serial_manager.init();
    gcs().setup_console();

    // Register mavlink_delay_cb, which will run anytime you have
    // more than 5ms remaining in your call to hal.scheduler->delay
    hal.scheduler->register_delay_callback(mavlink_delay_cb_static, 5);

    // setup any board specific drivers
    BoardConfig.init();
#if HAL_WITH_UAVCAN
    BoardConfig_CAN.init();
#endif

    // initialise rc channels including setting mode
    rc().init();

    relay.init();

    // initialise notify system
    notify.init();
    notify_mode(*control_mode);

    init_rc_out_main();

    // keep a record of how many resets have happened. This can be
    // used to detect in-flight resets
    g.num_resets.set_and_save(g.num_resets+1);

    // init baro
    barometer.init();

    // initialise rangefinder
    rangefinder.set_log_rfnd_bit(MASK_LOG_SONAR);
    rangefinder.init(ROTATION_PITCH_270);

    ppds_motor_pod.init(serial_manager);

    // initialise battery monitoring
    battery.init();

    rpm_sensor.init();

    // setup telem slots with serial ports
    gcs().setup_uarts();

#if OSD_ENABLED == ENABLED
    osd.init();
#endif

#if LOGGING_ENABLED == ENABLED
    log_init();
#endif

    // initialise airspeed sensor
    airspeed.init();

    AP::compass().set_log_bit(MASK_LOG_COMPASS);
    AP::compass().init();

#if OPTFLOW == ENABLED
    // make optflow available to libraries
    if (optflow.enabled()) {
        ahrs.set_optflow(&optflow);
    }
#endif

    // give AHRS the airspeed sensor
    ahrs.set_airspeed(&airspeed);

    // GPS Initialization
    gps.set_log_gps_bit(MASK_LOG_GPS);
    gps.init(serial_manager);

    init_rc_in();               // sets up rc channels from radio

#if MOUNT == ENABLED
    // initialise camera mount
    camera_mount.init();
#endif

#if LANDING_GEAR_ENABLED == ENABLED
    // initialise landing gear position
    g2.landing_gear.init();
    gear.last_auto_cmd = -1;
    gear.last_cmd = -1;
#endif

#if FENCE_TRIGGERED_PIN > 0
    hal.gpio->pinMode(FENCE_TRIGGERED_PIN, HAL_GPIO_OUTPUT);
    hal.gpio->write(FENCE_TRIGGERED_PIN, 0);
#endif

    /*
     *  setup the 'main loop is dead' check. Note that this relies on
     *  the RC library being initialised.
     */
    hal.scheduler->register_timer_failsafe(failsafe_check_static, 1000);

    quadplane.setup();

    AP_Param::reload_defaults_file(true);

    startup_ground();

    // don't initialise aux rc output until after quadplane is setup as
    // that can change initial values of channels
    init_rc_out_aux();

    // choose the nav controller
    set_nav_controller();

    set_mode_by_number((enum Mode::Number)g.initial_mode.get(), MODE_REASON_UNKNOWN);

    // set the correct flight mode
    // ---------------------------
    reset_control_switch();

    // initialise sensor
#if OPTFLOW == ENABLED
    if (optflow.enabled()) {
        optflow.init(-1);
    }
#endif

// init cargo gripper
#if GRIPPER_ENABLED == ENABLED
    g2.gripper.init();
#endif

    // disable safety if requested
    BoardConfig.init_safety();

#if AP_PARAM_KEY_DUMP
    AP_Param::show_all(hal.console, true);
#endif
}

//********************************************************************************
//This function does all the calibrations, etc. that we need during a ground start
//********************************************************************************
void Plane::startup_ground(void)
{
    set_mode(mode_initializing, MODE_REASON_UNKNOWN);

#if (GROUND_START_DELAY > 0)
    gcs().send_text(MAV_SEVERITY_NOTICE,"Ground start with delay");
    delay(GROUND_START_DELAY * 1000);
#else
    gcs().send_text(MAV_SEVERITY_INFO,"Ground start");
#endif

    //INS ground start
    //------------------------
    //
    startup_INS_ground();

    // Save the settings for in-air restart
    // ------------------------------------
    //save_EEPROM_groundstart();

    // initialise mission library
    mission.init();

    // initialise AP_Logger library
#if LOGGING_ENABLED == ENABLED
    logger.setVehicle_Startup_Writer(
        FUNCTOR_BIND(&plane, &Plane::Log_Write_Vehicle_Startup_Messages, void)
        );
#endif

#ifdef ENABLE_SCRIPTING
    if (!g2.scripting.init()) {
        gcs().send_text(MAV_SEVERITY_ERROR, "Scripting failed to start");
    }
#endif // ENABLE_SCRIPTING

    // reset last heartbeat time, so we don't trigger failsafe on slow
    // startup
    failsafe.last_heartbeat_ms = millis();

    // we don't want writes to the serial port to cause us to pause
    // mid-flight, so set the serial ports non-blocking once we are
    // ready to fly
    serial_manager.set_blocking_writes_all(false);

    gcs().send_text(MAV_SEVERITY_INFO,"Ground start complete");
}


bool Plane::set_mode(Mode &new_mode, const mode_reason_t reason)
{
    if (control_mode == &new_mode) {
        // don't switch modes if we are already in the correct mode.
        return true;
    }

#if !QAUTOTUNE_ENABLED
    if (&new_mode == &plane.mode_qautotune) {
        gcs().send_text(MAV_SEVERITY_INFO,"QAUTOTUNE disabled");
        set_mode(plane.mode_qhover, MODE_REASON_UNAVAILABLE);
        return false;
    }
#endif

    // backup current control_mode and previous_mode
    Mode &old_previous_mode = *previous_mode;
    Mode &old_mode = *control_mode;
    const mode_reason_t previous_mode_reason_backup = previous_mode_reason;

    // update control_mode assuming success
    // TODO: move these to be after enter() once start_command_callback() no longer checks control_mode
    previous_mode = control_mode;
    control_mode = &new_mode;
    previous_mode_reason = control_mode_reason;
    control_mode_reason = reason;

    // attempt to enter new mode
    if (!new_mode.enter()) {
        // Log error that we failed to enter desired flight mode
        gcs().send_text(MAV_SEVERITY_WARNING, "Flight mode change failed");

        // we failed entering new mode, roll back to old
        previous_mode = &old_previous_mode;
        control_mode = &old_mode;

        control_mode_reason = previous_mode_reason;
        previous_mode_reason = previous_mode_reason_backup;

        // currently, only Q modes can fail enter(). This will likely change in the future and all modes
        // should be changed to check dependencies and fail early before depending on changes in Mode::set_mode()
        if (control_mode->is_vtol_mode()) {
            // ignore result because if we fail we risk looping at the qautotune check above
            control_mode->enter();
        }
        return false;
    }

    if (previous_mode == &mode_autotune) {
        // restore last gains
        autotune_restore();
    }

    // exit previous mode
    old_mode.exit();

    // record reasons
    previous_mode_reason = control_mode_reason;
    control_mode_reason = reason;

    // log and notify mode change
    logger.Write_Mode(control_mode->mode_number(), control_mode_reason);
    notify_mode(*control_mode);
    gcs().send_message(MSG_HEARTBEAT);

    return true;
}

bool Plane::set_mode_by_number(const Mode::Number new_mode_number, const mode_reason_t reason)
{
    Mode *new_mode = plane.mode_from_mode_num(new_mode_number);
    if (new_mode == nullptr) {
        gcs().send_text(MAV_SEVERITY_INFO, "Error: invalid mode number: %d", new_mode_number);
        return false;
    }
    return set_mode(*new_mode, reason);
}

void Plane::check_long_failsafe()
{
    uint32_t tnow = millis();
    // only act on changes
    // -------------------
    if (failsafe.state != FAILSAFE_LONG && failsafe.state != FAILSAFE_GCS && flight_stage != AP_Vehicle::FixedWing::FLIGHT_LAND) {
        uint32_t radio_timeout_ms = failsafe.last_valid_rc_ms;
        if (failsafe.state == FAILSAFE_SHORT) {
            // time is relative to when short failsafe enabled
            radio_timeout_ms = failsafe.short_timer_ms;
        }
        if (failsafe.rc_failsafe &&
            (tnow - radio_timeout_ms) > g.fs_timeout_long*1000) {
            failsafe_long_on_event(FAILSAFE_LONG, MODE_REASON_RADIO_FAILSAFE);
        } else if (g.gcs_heartbeat_fs_enabled == GCS_FAILSAFE_HB_AUTO && control_mode == &mode_auto &&
                   failsafe.last_heartbeat_ms != 0 &&
                   (tnow - failsafe.last_heartbeat_ms) > g.fs_timeout_long*1000) {
            failsafe_long_on_event(FAILSAFE_GCS, MODE_REASON_GCS_FAILSAFE);
        } else if (g.gcs_heartbeat_fs_enabled == GCS_FAILSAFE_HEARTBEAT &&
                   failsafe.last_heartbeat_ms != 0 &&
                   (tnow - failsafe.last_heartbeat_ms) > g.fs_timeout_long*1000) {
            failsafe_long_on_event(FAILSAFE_GCS, MODE_REASON_GCS_FAILSAFE);
        } else if (g.gcs_heartbeat_fs_enabled == GCS_FAILSAFE_HB_RSSI &&
                   gcs().chan(0) != nullptr &&
                   gcs().chan(0)->last_radio_status_remrssi_ms != 0 &&
                   (tnow - gcs().chan(0)->last_radio_status_remrssi_ms) > g.fs_timeout_long*1000) {
            failsafe_long_on_event(FAILSAFE_GCS, MODE_REASON_GCS_FAILSAFE);
        }
    } else {
        uint32_t timeout_seconds = g.fs_timeout_long;
        if (g.fs_action_short != FS_ACTION_SHORT_DISABLED) {
            // avoid dropping back into short timeout
            timeout_seconds = g.fs_timeout_short;
        }
        // We do not change state but allow for user to change mode
        if (failsafe.state == FAILSAFE_GCS &&
            (tnow - failsafe.last_heartbeat_ms) < timeout_seconds*1000) {
            failsafe_long_off_event(MODE_REASON_GCS_FAILSAFE);
        } else if (failsafe.state == FAILSAFE_LONG &&
                   !failsafe.rc_failsafe) {
            failsafe_long_off_event(MODE_REASON_RADIO_FAILSAFE);
        }
    }
}

void Plane::check_short_failsafe()
{
    // only act on changes
    // -------------------
    if (g.fs_action_short != FS_ACTION_SHORT_DISABLED &&
       failsafe.state == FAILSAFE_NONE &&
       flight_stage != AP_Vehicle::FixedWing::FLIGHT_LAND) {
        // The condition is checked and the flag rc_failsafe is set in radio.cpp
        if(failsafe.rc_failsafe) {
            failsafe_short_on_event(FAILSAFE_SHORT, MODE_REASON_RADIO_FAILSAFE);
        }
    }

    if(failsafe.state == FAILSAFE_SHORT) {
        if(!failsafe.rc_failsafe || g.fs_action_short == FS_ACTION_SHORT_DISABLED) {
            failsafe_short_off_event(MODE_REASON_RADIO_FAILSAFE);
        }
    }
}


void Plane::startup_INS_ground(void)
{
#if HIL_SUPPORT
    if (g.hil_mode == 1) {
        while (barometer.get_last_update() == 0) {
            // the barometer begins updating when we get the first
            // HIL_STATE message
            gcs().send_text(MAV_SEVERITY_WARNING, "Waiting for first HIL_STATE message");
            hal.scheduler->delay(1000);
        }
    }
#endif

    if (ins.gyro_calibration_timing() != AP_InertialSensor::GYRO_CAL_NEVER) {
        gcs().send_text(MAV_SEVERITY_ALERT, "Beginning INS calibration. Do not move plane");
    } else {
        gcs().send_text(MAV_SEVERITY_ALERT, "Skipping INS calibration");
    }

    ahrs.init();
    ahrs.set_fly_forward(true);
    ahrs.set_vehicle_class(AHRS_VEHICLE_FIXED_WING);
    ahrs.set_wind_estimation(true);

    ins.init(scheduler.get_loop_rate_hz());
    ahrs.reset();

    // read Baro pressure at ground
    //-----------------------------
    barometer.set_log_baro_bit(MASK_LOG_IMU);
    barometer.calibrate();

    if (airspeed.enabled()) {
        // initialize airspeed sensor
        // --------------------------
        airspeed.calibrate(true);
    } else {
        gcs().send_text(MAV_SEVERITY_WARNING,"No airspeed");
    }
}

// sets notify object flight mode information
void Plane::notify_mode(const Mode& mode)
{
    notify.flags.flight_mode = mode.mode_number();
    notify.set_flight_mode_str(mode.name4());
}

/*
  should we log a message type now?
 */
bool Plane::should_log(uint32_t mask)
{
#if LOGGING_ENABLED == ENABLED
    return logger.should_log(mask);
#else
    return false;
#endif
}

/*
  return throttle percentage from 0 to 100 for normal use and -100 to 100 when using reverse thrust
 */
int8_t Plane::throttle_percentage(void)
{
    if (quadplane.in_vtol_mode()) {
        return quadplane.throttle_percentage();
    }
    float throttle = SRV_Channels::get_output_scaled(SRV_Channel::k_throttle);
    if (!have_reverse_thrust()) {
        return constrain_int16(throttle, 0, 100);
    }
    return constrain_int16(throttle, -100, 100);
}

// Adds a loiter to altitude waypoint between a MAV_CMD_DO_LAND_START and MAV_CMD_NAV_LAND command in the mission plan
// that lands the plane into wind with an approach entry and glide-slope specified by parameters.
// Returns true if the landing sequence was updated
// Returns false if the  landing sequence was not updated
bool Plane::create_into_wind_landing_sequence()
{

    // Basic check that there is a DO_LAND_START followed by a MAV_CMD_LAND waypoint
    uint16_t landing_start_index = plane.mission.get_landing_sequence_start();
    bool landing_start_defined = landing_start_index > 0;
    AP_Mission::Mission_Command land_point_cmd = {};
    int16_t land_point_index = plane.mission.get_next_land_cmd(landing_start_index, land_point_cmd);
    bool landing_point_defined = land_point_index > landing_start_index;

    if (!landing_start_defined || !landing_point_defined) {
        gcs().send_text(MAV_SEVERITY_DEBUG, "IWL mission commands not found\n");
        return false;
    }

    // Check that a MAV_CMD_NAV_LAND command either immediately follows the MAV_CMD_DO_LAND_START command
    // indicating which indicates that an an approach entry waypoint needs to be generated or is located at
    // the next index, indicating that the previously generated approach entry needs to be recalculated.
    uint16_t const desired_loiter_index = landing_start_index + 1;
    uint16_t const desired_land_index = landing_start_index + 2;
    if (land_point_index == desired_loiter_index) {
        gcs().send_text(MAV_SEVERITY_DEBUG, "IWL creating approach waypoint %i\n", desired_loiter_index);
    } else if (land_point_index == desired_land_index) {
        // Check that the waypoint between this and the DO_LAND_START has been previously modified
        // Otherwise respect the original mission plan
        AP_Mission::Mission_Command intermediate_point_cmd = {};
        bool imtermediate_point_exists = plane.mission.get_cmd(desired_loiter_index, intermediate_point_cmd);
        if ((imtermediate_point_exists && intermediate_point_cmd.is_modified) ||
                !plane.mission.is_nav_cmd(intermediate_point_cmd)) {
            gcs().send_text(MAV_SEVERITY_DEBUG, "IWL updating approach waypoint %i\n", desired_loiter_index);
        } else {
            gcs().send_text(MAV_SEVERITY_DEBUG, "IWL respecting loaded approach waypoint\n");
            return false;
        }
    } else {
        gcs().send_text(MAV_SEVERITY_DEBUG, "IWL land point index invalid\n");
        return false;
    }

    // get unit wind vector
    Vector3f windUnitVec = ahrs.wind_estimate();
    windUnitVec.z = 0.0f;
    windUnitVec.normalize();

    // Get desired approach heading and constrain if necessary
    // During landing loiter_xtrack is used to indicate if heading constraint data is available
    // The desired approach heading and tolerance are held in the p1 parameter
    float turn_radius = (float)aparm.loiter_radius; // positive is CW
    if (plane.mission.isAngleSectorLanding(land_point_cmd)) {
        uint16_t sector_yaw_deg_uint;
        uint16_t tolerance_deg_uint;
        plane.mission.unpackAngleSectorParam(land_point_cmd.p1,  sector_yaw_deg_uint, tolerance_deg_uint);
        if (tolerance_deg_uint < 180) {
            float sector_yaw_deg = wrap_180((float)sector_yaw_deg_uint);
            float sector_yaw_rad = radians(sector_yaw_deg);
            float approach_iwl_hdg_rad = atan2f(-windUnitVec.y,-windUnitVec.x);
            float approach_hdg_tol_rad = radians((float)tolerance_deg_uint);
            if (wrap_PI(approach_iwl_hdg_rad - sector_yaw_rad) > approach_hdg_tol_rad) {
                // clip to upper limit
                approach_iwl_hdg_rad = sector_yaw_rad + approach_hdg_tol_rad;
                windUnitVec.x = -cosf(approach_iwl_hdg_rad);
                windUnitVec.y = -sinf(approach_iwl_hdg_rad);
            } else if (wrap_PI(approach_iwl_hdg_rad - sector_yaw_rad) < -approach_hdg_tol_rad) {
                // clip to lower limit
                approach_iwl_hdg_rad = sector_yaw_rad - approach_hdg_tol_rad;
                windUnitVec.x = -cosf(approach_iwl_hdg_rad);
                windUnitVec.y = -sinf(approach_iwl_hdg_rad);
            }

            // Set turn direction based on which side of the approach sector we are using
            // to minimise excusion outside the sector when turning onto final approach
            if (wrap_PI(approach_iwl_hdg_rad - sector_yaw_rad) > 0.0f) {
                // approaching from the left side of the sector, so use a right turn onto final (positive radius)
                if (turn_radius < 0.0f) {
                    turn_radius = -turn_radius;
                }
            } else {
                // approaching from the right side of the sector, so use a left  turn onto final (negative radius)
                if (turn_radius > 0.0f) {
                    turn_radius = -turn_radius;
                }
            }
        }
    }

    // calculate offset from landing point to centre of a loiter to altitude waypoint that feeds the aircraft into the approach
    const float approach_length = 100.0f * (float)MAX(plane.g.wal_start_height, 10) / (float)MAX(plane.g.wal_approach_gradient_pct, 5);
    Vector2f offsetNE;
    offsetNE.x = approach_length * windUnitVec.x + turn_radius * windUnitVec.y;
    offsetNE.y = approach_length * windUnitVec.y - turn_radius * windUnitVec.x;

    // write the land waypoint with space for one intermediate waypoint between it and the DO_LAND_START
    if (desired_land_index >= plane.mission.num_commands()) {
        if (!plane.mission.add_cmd(land_point_cmd)) {
            gcs().send_text(MAV_SEVERITY_DEBUG, "IWL land point write failed\n");
            return false;
        }
    } else {
        if (!plane.mission.replace_cmd(desired_land_index, land_point_cmd)) {
            gcs().send_text(MAV_SEVERITY_DEBUG, "IWL land point write failed\n");
            return false;
        }
    }

    // create an intermediate loiter to altitude command
    AP_Mission::Mission_Command land_loiter_cmd = land_point_cmd;
    land_loiter_cmd.content.location.alt += (int32_t)(100 * plane.g.wal_start_height);
    land_loiter_cmd.id = MAV_CMD_NAV_LOITER_TO_ALT;
    land_loiter_cmd.p1 = fabsf(turn_radius); // radius in metres
    land_loiter_cmd.content.location.loiter_ccw = turn_radius < 0;
    land_loiter_cmd.content.location.loiter_xtrack = 1; // xtrack from tangent exit location
    land_loiter_cmd.is_modified = true;

    // move the location to set up the correct approach path
    land_loiter_cmd.content.location.offset(offsetNE.x, offsetNE.y);
    if (!plane.mission.replace_cmd(desired_loiter_index, land_loiter_cmd)) {
        gcs().send_text(MAV_SEVERITY_DEBUG, "IWL approach point write failed\n");
        return false;
    }

    // set index and restart command
    plane.mission.set_current_cmd(landing_start_index+1);

    gcs().send_text(MAV_SEVERITY_ALERT, "IWL from %d m and %d deg\n", (int)approach_length, (int)degrees(atan2f(windUnitVec.y,windUnitVec.x)));

    return true;
}