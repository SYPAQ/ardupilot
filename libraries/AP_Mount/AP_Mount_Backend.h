/*
   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
  Mount driver backend class. Each supported mount type
  needs to have an object derived from this class.
 */
#pragma once

#include <AP_Common/AP_Common.h>
#include "AP_Mount.h"
#include <AP_AHRS/AP_AHRS_View.h>

class AP_Mount_Backend
{
public:
    // Constructor
    AP_Mount_Backend(AP_Mount &frontend, AP_Mount::mount_state& state, uint8_t instance) :
        _frontend(frontend),
        _state(state),
        _instance(instance)
    {}

    // Virtual destructor
    virtual ~AP_Mount_Backend(void) {}

    // init - performs any required initialisation for this instance
    virtual void init(const AP_SerialManager& serial_manager) = 0;

    // update mount position - should be called periodically
    virtual void update() = 0;

    // used for gimbals that need to read INS data at full rate
    virtual void update_fast() {}

    // has_pan_control - returns true if this mount can control it's pan (required for multicopters)
    virtual bool has_pan_control() const = 0;

    // return the earth frame yaw of the payload in radians
    virtual float get_ef_yaw();

    // set_mode - sets mount's mode
    virtual void set_mode(enum MAV_MOUNT_MODE mode) = 0;

    // set_angle_targets - sets angle targets in degrees
    virtual void set_angle_targets(float roll, float tilt, float pan);

    // set yaw target in degrees
    virtual void set_yaw_target(float pan);

    // specialised mode that uses RC targeting
    // when called with park = true, gimbal is held at last demanded earth frame elevation angle, roll is held to zero and yaw moves with vehicle yaw
    // when called with park = false, causes the mount to revert to normal RC targeting operation
    virtual void set_elev_park(bool park);

    // reset the mount LOS elevation angle to the parameter defined value
    virtual void reset_elev();

    // set_roi_target - sets target location that mount should attempt to point towards and its NE velocity
    virtual void set_roi_target(const struct Location &target_loc, Vector2f &roi_velNE);

    // get_roi_target - gets target location that mount should attempt to point towards
    virtual Location get_roi_target();

    // control - control the mount
    virtual void control(int32_t pitch_or_lat, int32_t roll_or_lon, int32_t yaw_or_alt, MAV_MOUNT_MODE mount_mode);
    
    // process MOUNT_CONFIGURE messages received from GCS:
    void handle_mount_configure(const mavlink_mount_configure_t &msg);

    // process MOUNT_CONTROL messages received from GCS:
    void handle_mount_control(const mavlink_mount_control_t &packet);

    // send_mount_status - called to allow mounts to send their status to GCS via MAVLink
    virtual void send_mount_status(mavlink_channel_t chan) = 0;

    // handle a GIMBAL_REPORT message
    virtual void handle_gimbal_report(mavlink_channel_t chan, const mavlink_message_t *msg) {}

    // handle a PARAM_VALUE message
    virtual void handle_param_value(const mavlink_message_t *msg) {}

    // send a GIMBAL_REPORT message to the GCS
    virtual void send_gimbal_report(const mavlink_channel_t chan) {}

protected:

    // update_targets_from_rc - updates angle targets (i.e. _angle_ef_target_rad) using input from receiver
    void update_targets_from_rc();

    // angle_input, angle_input_rad - convert RC input into an earth-frame target angle
    int32_t angle_input(const RC_Channel* rc, int16_t angle_min, int16_t angle_max);
    float angle_input_rad(const RC_Channel* rc, int16_t angle_min, int16_t angle_max);

    // calc_angle_to_location - calculates the earth-frame roll, tilt and pan angles (and radians) to point at the given target
    void calc_angle_to_location(const struct Location &target, Vector3f& angles_to_target_rad, bool calc_tilt, bool calc_pan, bool relative_pan = true);

    // get the mount mode from frontend
    MAV_MOUNT_MODE get_mode(void) const { return _frontend.get_mode(_instance); }

    AP_Mount    &_frontend; // reference to the front end which holds parameters
    AP_Mount::mount_state &_state;    // references to the parameters and state for this backend
    uint8_t     _instance;  // this instance's number
    Vector3f    _angle_ef_target_rad;   // desired earth-frame roll, tilt and vehicle-relative pan angles in radians
    bool _slave_yaw_roll = false; // when set to true the earth frame yaw angle aligns with vehicle yaw and roll is set to zero

private:

    void rate_input_rad(float &out, const RC_Channel *ch, int16_t min = 0, int16_t max = 0) const;
};
