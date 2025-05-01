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

## Dependencies & Building OpenCV with CUDA

This project relies heavily on OpenCV with CUDA acceleration enabled for performance. You **cannot** typically use pre-built OpenCV binaries; you must compile OpenCV from source with CUDA support tailored to your specific GPU hardware and installed CUDA Toolkit version.

**Required OpenCV DLLs (Version 4.11.0):**

- `opencv_core4110.dll`
- `opencv_imgproc4110.dll`
- `opencv_cudaarithm4110.dll`
- `opencv_cudafilters4110.dll`
- `opencv_cudaimgproc4110.dll`
- `opencv_world4110.dll` (May be present depending on build configuration)

**General Guide to Building OpenCV with CUDA:**

Building OpenCV with CUDA is a complex process and requires careful configuration. This is a high-level overview; refer to the official OpenCV documentation for detailed, up-to-date instructions.

1.  **Prerequisites:**

    - **NVIDIA GPU:** A CUDA-compatible NVIDIA graphics card.
    - **NVIDIA CUDA Toolkit:** Install the version compatible with your GPU driver and the desired OpenCV version. Ensure `nvcc` (the CUDA compiler) is in your system's PATH.
    - **NVIDIA cuDNN:** (Optional but recommended for deep learning modules, might be needed by some OpenCV CUDA features). Download and install the version matching your CUDA Toolkit version.
    - **CMake:** The cross-platform build system generator.
    - **Supported C++ Compiler:** Visual Studio (on Windows) with the C++ development workload installed.
    - **OpenCV Source Code:** Download the source code for the desired OpenCV version (e.g., 4.11.0) and the corresponding `opencv_contrib` repository (for extra modules, often needed).

2.  **Configuration (CMake):**

    - Run CMake GUI or command-line CMake.
    - Set the source code directory to the OpenCV source folder.
    - Set the build directory to a new, empty folder (e.g., `opencv/build`).
    - Click "Configure". Select your compiler (e.g., Visual Studio 17 2022, x64).
    - Search for and enable the `WITH_CUDA` option.
    - CMake should attempt to automatically detect your CUDA Toolkit. Verify the paths (`CUDA_TOOLKIT_ROOT_DIR`).
    - You may need to manually set `OPENCV_EXTRA_MODULES_PATH` to the `modules` directory inside the downloaded `opencv_contrib` source folder.
    - Configure CUDA-specific options if needed (e.g., `CUDA_ARCH_BIN` to specify your GPU's compute capability). CMake usually detects this, but verify. Common values might be `7.5`, `8.6`, etc. You can specify multiple.
    - Enable other desired modules (e.g., `BUILD_opencv_world` if you want a single large DLL, though separate DLLs are often preferred for development). Ensure the required modules (`core`, `imgproc`, `cudaarithm`, `cudafilters`, `cudaimgproc`) are enabled.
    - Disable modules you don't need to speed up compilation (e.g., `BUILD_TESTS`, `BUILD_PERF_TESTS`, `BUILD_EXAMPLES`).
    - Click "Configure" again, then "Generate".

3.  **Compilation (Visual Studio):**

    - Open the generated `OpenCV.sln` file (located in your build directory) in Visual Studio.
    - Select the desired build configuration (e.g., `Release`, `x64`).
    - Build the `ALL_BUILD` target. This will compile the libraries.
    - Build the `INSTALL` target. This will copy the compiled DLLs, LIBs, and header files to the installation directory specified in CMake (`CMAKE_INSTALL_PREFIX`).

4.  **Integration:**
    - Configure your Visual Studio project (this aimbot project) to link against the newly built OpenCV libraries:
      - Add the OpenCV `include` directory to your project's "Additional Include Directories".
      - Add the OpenCV `lib` directory (containing `.lib` files) to your project's "Additional Library Directories".
      - Link the required `.lib` files (e.g., `opencv_core4110.lib`, `opencv_cudaimgproc4110.lib`, etc.) in "Additional Dependencies".
    - Ensure the compiled OpenCV DLLs are either in the same directory as your executable or in a location included in the system's PATH environment variable so they can be found at runtime.

**Note:** This process can be time-consuming and prone to errors. Carefully check compatibility between your GPU driver, CUDA Toolkit, cuDNN, and the OpenCV version. Consult online guides and the official documentation frequently.

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
