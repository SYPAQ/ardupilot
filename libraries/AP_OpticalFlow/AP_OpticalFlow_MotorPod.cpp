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
  driver for MotorPod Pixart PAW3903E1 optical flow sensor
 */

#include <AP_HAL/AP_HAL.h>
#include <AP_AHRS/AP_AHRS.h>
#include "OpticalFlow.h"
#include "AP_OpticalFlow_MotorPod.h"
#include "AP_PpdsMotorPod/AP_PpdsMotorPod.hpp"
#include <stdio.h>

// TODO: this is a bit of a hack. Is there a better way?
// For this driver to work, the AP_PpdsMotorPod driver needs to be included for
// compilation. As we don't have feature flags or other conditional compilation,
// we add weak versions of the required AP_PpdsMotorPod functions to allow this
// to compile without the AP_PpdsMotorPod driver.
namespace AP {
    WEAK AP_PpdsMotorPod *motorPod() {
        return nullptr;
    }
}
WEAK bool AP_PpdsMotorPod::getFlowData(FlowData * const flow_data) { return false; }
WEAK void AP_PpdsMotorPod::clearFlowData(void) { /* empty */ }


AP_OpticalFlow_MotorPod::AP_OpticalFlow_MotorPod(OpticalFlow &_frontend) :
    OpticalFlow_backend(_frontend)
{
    // empty
}


// detect the device
AP_OpticalFlow_MotorPod *AP_OpticalFlow_MotorPod::detect(OpticalFlow &_frontend) {
    if (AP::motorPod() != nullptr) {
        AP_OpticalFlow_MotorPod *sensor = new AP_OpticalFlow_MotorPod(_frontend);
        if (sensor) {
            return sensor;
        }
    }
    return nullptr;
}

void AP_OpticalFlow_MotorPod::update(void) {
    if (AP::motorPod() == nullptr) {
        return;
    }

    WITH_SEMAPHORE(_sem);

    const uint32_t kNow_us = AP_HAL::micros();
    const uint32_t kUpdatePeriod_us = (kNow_us - this->lastUpdate_us);
    this->lastUpdate_us = kNow_us;

    const Vector3f& kGyro_vec = AP::ahrs_navekf().get_gyro();
    // accumulate gyro data
    this->gyro_accum.x += kGyro_vec.x;
    this->gyro_accum.y += kGyro_vec.y;
    this->gyro_accum.t += kUpdatePeriod_us;

    // Get optical flow data from MotorPod driver
    AP_PpdsMotorPod::FlowData flowData;
    bool gotData = AP::motorPod()->getFlowData(&flowData);

    // return without updating state if no readings
    if (!gotData) {
        return;
    }

    AP::motorPod()->clearFlowData();

    struct OpticalFlow::OpticalFlow_state state;
    state.surface_quality = flowData.surfaceQuality;

    const float kMicrosToSeconds = 1.0e-6;
    float delta_t_flow = flowData.delta.t_us * kMicrosToSeconds;
    float delta_t_gyro = this->gyro_accum.t * kMicrosToSeconds;

    // sanity check the values from the Motor Pod
    if (fabs(delta_t_flow * 0.9f) > delta_t_gyro) {
        delta_t_flow = -1.0;
    }

    if (is_positive(delta_t_flow)) {

        const Vector2f flowScaler = _flowScaler();
        float flowScaleFactorX = 1.0f + 0.001f * flowScaler.x;
        float flowScaleFactorY = 1.0f + 0.001f * flowScaler.y;

        // TODO: Work out why we need to invert the flow rate x-axis
        state.flowRate = Vector2f(flowData.delta.x * flowScaleFactorX * -1.0,
                                  flowData.delta.y * flowScaleFactorY);
        state.flowRate *= kFlowPixelScaling / delta_t_flow;

        state.bodyRate = Vector2f(this->gyro_accum.x / delta_t_gyro,
                                  this->gyro_accum.y / delta_t_gyro);

        // clear the accumulator after we use the data
        this->gyro_accum.x = 0;
        this->gyro_accum.y = 0;
        this->gyro_accum.t = 0;

        // we only apply yaw to flowRate as body rate comes from AHRS
        _applyYaw(state.flowRate);
    } else {
        state.flowRate.zero();
        state.bodyRate.zero();
    }

    // copy results to front end
    _update_frontend(state);
}
