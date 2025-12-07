# PSVR2 EyeTracking Calibration

This is the client application for PSVR2 eye tracking calibration. The generated calibration data is applied **in real-time** by the modified PSVR2Toolkit. There is **NO** need to restart SteamVR to apply new calibration offsets. This project is adapted from the [OpenXR Samples for Mixed Reality Developers](https://github.com/microsoft/OpenXR-MixedReality-Samples) by Microsoft.

## Important Notes & Compatibility

### Compatibility Warning
This calibration tool is designed to work ONLY with the **Modified PSVR2Toolkit Driver** available at:
**[https://github.com/tobexeon/PSVR2Toolkit-Custom](https://github.com/tobexeon/PSVR2Toolkit-Custom)**

It is **NOT** compatible with the original PSVR2Toolkit by BnuuySolutions, as the calibration IPC interface is exclusive to the modified driver.

## Usage
1. Run PSVR2EyeTrackingCalibration.exe with SteamVR active.
2. Follow the on-screen instructions (look at the red dot and pull the right trigger).
3. The calibration offset is saved to %USERPROFILE%\Documents\PSVR2Calibration.txt
