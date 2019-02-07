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
  reverse thrust support functions
 */
#include "Plane.h"

/*
  see if reverse thrust should be allowed in the current flight state
 */
bool Plane::allow_reverse_thrust(void) const
{
    // check if we should allow reverse thrust
    bool allow = false;

    if (g.use_reverse_thrust == USE_REVERSE_THRUST_NEVER || !have_reverse_thrust()) {
        return false;
    }

    switch (control_mode) {
    case AUTO:
        {
        uint16_t nav_cmd = mission.get_current_nav_cmd().id;

        // never allow reverse thrust during takeoff
        if (nav_cmd == MAV_CMD_NAV_TAKEOFF) {
            return false;
        }

        // always allow regardless of mission item
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_AUTO_ALWAYS);

        // landing
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_AUTO_LAND_APPROACH) &&
                (nav_cmd == MAV_CMD_NAV_LAND);

        // LOITER_TO_ALT
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_AUTO_LOITER_TO_ALT) &&
                (nav_cmd == MAV_CMD_NAV_LOITER_TO_ALT);

        // any Loiter (including LOITER_TO_ALT)
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_AUTO_LOITER_ALL) &&
                    (nav_cmd == MAV_CMD_NAV_LOITER_TIME ||
                     nav_cmd == MAV_CMD_NAV_LOITER_TO_ALT ||
                     nav_cmd == MAV_CMD_NAV_LOITER_TURNS ||
                     nav_cmd == MAV_CMD_NAV_LOITER_UNLIM);

        // waypoints
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_AUTO_WAYPOINT) &&
                    (nav_cmd == MAV_CMD_NAV_WAYPOINT ||
                     nav_cmd == MAV_CMD_NAV_SPLINE_WAYPOINT);
        }
        break;

    case LOITER:
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_LOITER);
        break;
    case RTL:
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_RTL);
        break;
    case CIRCLE:
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_CIRCLE);
        break;
    case CRUISE:
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_CRUISE);
        break;
    case FLY_BY_WIRE_B:
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_FBWB);
        break;
    case AVOID_ADSB:
    case GUIDED:
        allow |= (g.use_reverse_thrust & USE_REVERSE_THRUST_GUIDED);
        break;
    default:
        // all other control_modes are auto_throttle_mode=false.
        // If we are not controlling throttle, don't limit it.
        allow = true;
        break;
    }

    return allow;
}

/*
  return true if we are configured to support reverse thrust
 */
bool Plane::have_reverse_thrust(void) const
{
    return aparm.throttle_min < 0;
}

/*
  return control in from the radio throttle channel.
 */
int16_t Plane::get_throttle_input(bool no_deadzone) const
{
    int16_t ret;
    if (quadplane.tailsitter.input_type == quadplane.TAILSITTER_CORVOX && RC_Channels::has_active_overrides()
            && (control_mode == FLY_BY_WIRE_B || control_mode == CRUISE || control_mode == AUTO || control_mode == RTL || control_mode == LOITER)) {
        // handle special case where corvo hand controller is being used where the pitch stick is used to accel/decel the vehicle
        // forward/down stick is positive/faster
        ret = (int16_t)(-100.0f * (float)channel_pitch->norm_input_dz());
    } else {
        if (no_deadzone) {
            ret = channel_throttle->get_control_in_zero_dz();
        } else {
            ret = channel_throttle->get_control_in();
        }
        if (reversed_throttle) {
            // RC option for reverse throttle has been set
            ret = -ret;
        }
    }
    return ret;
}
