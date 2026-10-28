# Dream Air SteamVR Eye Calibration

for the sboys3 driver.

Download **SteamVREyeCalibrator.exe** from the
[latest release](https://github.com/WitherBlitz/Dream-Air-SteamVR-Eye-Calibration/releases/latest).

Double-click **SteamVREyeCalibrator.exe** with Pimax EVO/Play and SteamVR running.
Wear the Dream Air and close the SteamVR dashboard. Pimax's calibration interface
appears directly in the headset; you do not need SteamVR's Desktop view.
Your VR application can remain open while the calibration overlay is displayed.

Press **A**, **trigger**, or **Enter** to begin the vendor calibration routine.
Follow its dots with your eyes. The desktop helper also has a **Calibrate / Enter**
button. Keep the Pimax calibration window open and unminimized. Windows may show a
capture border around that window. The utility captures only that application.

If Windows prevents controller input from focusing Pimax, click Pimax's desktop
window once and press Enter. Use Pimax's own success/failure screen to assess the
result. Close the helper to exit. An existing Pimax calibrator is left running;
a calibrator launched by this helper receives a normal window-close request.

This is a **native SteamVR presentation bridge for the installed Pimax calibrator**.
It does not implement a replacement eye-tracking model, alter Pimax's licensing,
or distribute OEM binaries. Pimax's original program collects and applies the
calibration. The overlay preserves the inspected scene's 60-degree vertical FOV
and two-metre target depth; it is not a freely resized virtual desktop.

## Requirements and verification limits

- Windows 10 2004+ / Windows 11, x64, an active SteamVR headset and Pimax software.
- Pimax installed under Program Files/Pimax, SteamVR in Steam's default library.
- The inspected Pimax EVO 1.0.1.103 calibration scene/code. The helper checks their
  hashes and refuses unknown versions to avoid silently misaligning gaze targets.
- Display over a running VR application was confirmed in the headset. The vendor
  calibration engine must still be able to initialize its Pimax runtime services.
- The native capture/SteamVR path can be smoke-tested without starting calibration.
  Eye-calibration accuracy and persistence must be checked by wearing the headset,
  completing Pimax's routine, then testing gaze in the application you use.

Logs: `%LOCALAPPDATA%\SteamVREyeCalibrator\session.log`.
Read-only runtime check: `SteamVREyeCalibrator.exe --diagnose` (`diagnostic.log`).
Capture/overlay test: `SteamVREyeCalibrator.exe --smoke-test` (`smoke.log` and
`smoke-capture.png`). The smoke test never presses Enter or starts eye calibration.

## Build

Run `powershell -NoProfile -File build.ps1` with MSVC x64 build tools and a Windows
SDK including C++/WinRT. The script compiles/runs the core tests, then builds the
Windows executable with `/W4 /WX /MT`, written to `dist`. Build intermediates are
written to `.build`. Use `-OutputDirectory` and `-IntermediateDirectory` to choose
other directories. No Python, Node, or .NET runtime is needed.

The Valve header is pinned to OpenVR commit
`0924064316de3effbcd1acf1e309182a2deb1c05` (2.15.6), with SHA-256 verification in
the build script. See `vendor/LICENSE-OpenVR.txt` (also included beside the
executable) for its BSD license.
