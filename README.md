# Aimlabs Aimbot Project

This project is a C++ application developed as an aimbot, specifically targeting the Aimlabs training software environment. It leverages several technologies for its operation:

- **Screen Capture:** Utilizes the modern Windows Graphics Capture API (`Windows.Graphics.Capture`) for efficient, high-performance screen recording of the primary monitor.
- **Image Processing:** Employs the OpenCV library, heavily utilizing its CUDA modules (`cudaimgproc`, `cudafilters`, `cudaarithm`) for GPU-accelerated image processing. This includes:
  - Color space conversion (BGRA to BGR, BGR to HSV) performed on the GPU.
  - Color thresholding within a specific Region of Interest (ROI) around the screen center to isolate potential targets based on color (likely yellow/orange targets common in Aimlabs).
  - Morphological operations (opening) on the GPU to refine the target mask.
  - Contour detection on the CPU to identify distinct target shapes from the processed mask.
- **Target Detection & Selection:**
  - Filters detected contours based on a minimum area threshold.
  - Calculates the centroid (center point) of valid contours.
  - Implements a "target sticking" mechanism: If a target was detected in the previous frame, it prioritizes locking onto that same target if it remains within a defined proximity. Otherwise, it selects the valid contour closest to the calculated screen center (crosshair position).
- **Aim Assistance:**
  - Calculates the pixel distance between the screen center and the selected target's center.
  - Simulates relative mouse movement using the Windows `SendInput` API to automatically move the cursor towards the target. Includes a configurable smoothing factor (currently disabled).
- **Auto-Shooting:** If a target is detected and the aimbot is active, it rapidly simulates left mouse clicks via `SendInput`.
- **Concurrency:** Uses C++ standard library features (`std::thread`, `std::mutex`, `std::condition_variable`, `std::atomic`) to run screen capture, image processing, and shooting logic in separate threads for better performance and responsiveness.
- **Control:** The aimbot functionality can be toggled ON/OFF using the F8 key. The application can be terminated by pressing the Q key.
- **Stability Note:** This implementation is experimental. It may exhibit instability, inconsistent performance, or require further tuning depending on system configuration and specific Aimlabs scenarios.

## ⚠️ CAUTION: IMPORTANT WARNINGS ⚠️

**This software is intended for educational and research purposes ONLY.**

Using aimbots or any form of cheating software in online multiplayer games like Aimlabs (or others) is strictly against their Terms of Service (ToS).

**Using this software in online games WILL likely result in:**

- **Permanent account bans.**
- **Detection by anti-cheat systems.**
- **Negative impact on the gaming community.**

**By using or compiling this software, you acknowledge and agree that:**

1.  You understand the risks associated with using cheating software.
2.  You will **NOT** use this software in any online multiplayer environment or against other players.
3.  The author(s) and contributor(s) of this project are **NOT** responsible for any consequences resulting from the misuse of this software, including but not limited to account bans or legal action.
4.  You assume **ALL** responsibility for your actions related to this software.

**Use this software responsibly and ethically, solely for learning and experimentation in offline or controlled environments.**
