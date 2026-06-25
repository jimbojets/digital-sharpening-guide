#include "imu.h"

#ifndef UNIT_TEST
#include <M5Unified.h>

namespace imu {

FaultCode begin() {
    // Assumes M5.begin(cfg) was already invoked with cfg.internal_imu = true.
    if (!M5.Imu.isEnabled()) {
        return FaultCode::E01_BEGIN_FAILED;
    }

    // Sanity-read.
    Vec3 a, g;
    bool ok = false;
    for (int i = 0; i < 10; i++) {
        if (read(a, g)) { ok = true; break; }
        delay(5);
    }
    if (!ok) return FaultCode::E02_SELF_TEST_FAILED;

    // WHO_AM_I mismatch indicator updated for M5StickS3:
    // M5Unified's auto-detected type should now be m5::imu_bmi270.
    // If it yields any other value, a different board version was detected
    // or an internal I2C bus collision prevented the query from finishing.
    if (M5.Imu.getType() != m5::imu_bmi270) {
        return FaultCode::E03_WHO_AM_I_MISMATCH;
    }
    return FaultCode::NONE;
}

bool read(Vec3& accel_g, Vec3& gyro_dps) {
    float ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0;
    
    // In M5Unified's framework, getAccel and getGyro return false if no new data 
    // interrupt packet has populated the internal registries yet. This is standard 
    // behavior at 50 Hz. The function will safely output the last cached sample.
    bool a_ok = M5.Imu.getAccel(&ax, &ay, &az);
    bool g_ok = M5.Imu.getGyro (&gx, &gy, &gz);

    // --- S3 AXIS MAPPING VERIFICATION HOOK ---
    // If testing with your diag.cpp reveals that the physical board mounting on 
    // the S3 flips an axis compared to the original Plus, invert the signs here:
    // e.g. accel_g = {ax, -ay, az};
    // This protects your pure mathematical app.cpp filter layers from breaking.
    accel_g  = {ax, ay, az};
    gyro_dps = {gx, gy, gz};
    
    return a_ok && g_ok;
}

} // namespace imu

#else
// Native stub — tests don't exercise IMU; keep shared code compilable.
namespace imu {
    FaultCode begin() { return FaultCode::NONE; }
    bool read(Vec3& a, Vec3& g) { a = {0,0,-1}; g = {0,0,0}; return true; }
}
#endif