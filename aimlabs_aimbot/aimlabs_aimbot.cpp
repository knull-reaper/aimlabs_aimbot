#include <windows.h> // Ensure this is the very first line
#include <d3d11.h>
#include <dxgi1_2.h> // Keep for texture description format
#include <wrl/client.h> // Keep for ComPtr

// Suppress warnings from external OpenCV headers
#pragma warning(push, 0)
#include <opencv2/opencv.hpp>
#include <opencv2/imgproc.hpp>
// Add OpenCV CUDA includes
#include <opencv2/cudaimgproc.hpp>
#include <opencv2/cudafilters.hpp>
#include <opencv2/cudaarithm.hpp>
#pragma warning(pop)

// C++/WinRT Headers for Graphics Capture
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h> // For IGraphicsCaptureItemInterop
#include <windows.graphics.directx.direct3d11.interop.h> // For CreateDirect3D11DeviceFromDXGIDevice

#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cmath>
#include <vector>
#include <algorithm> // For std::max, std::min
#include <dwmapi.h> // For DwmGetWindowAttribute

// Link necessary libraries
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib") // Needed for DwmGetWindowAttribute

using Microsoft::WRL::ComPtr;
namespace winrt
{
    using namespace Windows::Foundation;
    using namespace Windows::Graphics;
    using namespace Windows::Graphics::Capture;
    using namespace Windows::Graphics::DirectX;
    using namespace Windows::Graphics::DirectX::Direct3D11;
}

namespace {

    // --- Constants ---
    const int SCREEN_WIDTH = 1920; // Reference, actual capture size used
    const int SCREEN_HEIGHT = 1080; // Reference, actual capture size used
    const int CROSSHAIR_X = SCREEN_WIDTH / 2; // Used for ROI calculation relative to screen center
    const int CROSSHAIR_Y = SCREEN_HEIGHT / 2; // Used for ROI calculation relative to screen center

    // ROI dimensions
    const int ROI_WIDTH = 1024;
    const int ROI_HEIGHT = 768;
    // Global ROI position (calculated based on capture size)
    std::atomic<int> g_roiLeft = CROSSHAIR_X - (ROI_WIDTH / 2);
    std::atomic<int> g_roiTop = CROSSHAIR_Y - (ROI_HEIGHT / 2);
    std::atomic<int> g_roiWidthActual = ROI_WIDTH; // Actual ROI width used
    std::atomic<int> g_roiHeightActual = ROI_HEIGHT; // Actual ROI height used
    // Capture dimensions
    std::atomic<int> g_captureWidth = SCREEN_WIDTH;
    std::atomic<int> g_captureHeight = SCREEN_HEIGHT;

    const int MOVEMENT_THRESHOLD = 1;
    const double AIM_SMOOTHING_FACTOR = 1.0; // Keep at 1.0 for testing stability/speed
    const double MIN_CONTOUR_AREA = 50.0; // Minimum area for a contour to be considered a target
    const double MAX_TARGET_STICK_DISTANCE = 15.0; // Max distance (pixels) to stick to previous target

    const int VK_KEY_Q = 0x51;
    const int VK_KEY_F8 = 0x77;

    // --- Global Variables for Thread Coordination ---
    std::mutex g_frameMutex;
    std::condition_variable g_frameCond;
    cv::Mat g_capturedFrame; // Stores the full captured frame (BGRA)
    std::atomic<bool> g_newFrame(false);
    std::atomic<bool> g_running(true);
    std::atomic<bool> g_aimbotActive(false);

    std::atomic<bool> g_targetDetected(false);
    std::atomic<int> g_targetX(0);
    std::atomic<int> g_targetY(0);
    cv::Point g_previousTargetPoint(-1, -1); // Store previous target point (relative to ROI)

    // --- Graphics Capture Specific Globals ---
    winrt::IDirect3DDevice g_winrtDevice{ nullptr };
    ComPtr<ID3D11Device> g_d3dDevice;
    ComPtr<ID3D11DeviceContext> g_d3dContext;
    winrt::Direct3D11CaptureFramePool g_framePool{ nullptr };
    winrt::GraphicsCaptureSession g_session{ nullptr };
    winrt::SizeInt32 g_lastFrameSize;
    winrt::event_token g_frameArrivedToken; // Token for FrameArrived event


    // --- Utility Functions ---
    static double calculateDistance(const cv::Point& p1, const cv::Point& p2) {
        return std::hypot(p1.x - p2.x, p1.y - p2.y);
    }

    static void simulateMouseClick(int /*x*/, int /*y*/) {
        INPUT inputs[2] = {};
        ZeroMemory(inputs, sizeof(inputs));
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        inputs[1].type = INPUT_MOUSE;
        inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(2, inputs, sizeof(INPUT));
    }

    static void simulateMouseMoveRelative(int dx, int dy) {
        INPUT input = {};
        ZeroMemory(&input, sizeof(input));
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        input.mi.dx = dx;
        input.mi.dy = dy;
        SendInput(1, &input, sizeof(INPUT));
    }

    // --- Helper to create Direct3D Device ---
    bool CreateDirect3DDevice() {
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
        UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
        //createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG; // Optional: Enable debug layer
#endif

        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
            &featureLevel, 1, D3D11_SDK_VERSION, g_d3dDevice.ReleaseAndGetAddressOf(), nullptr, g_d3dContext.ReleaseAndGetAddressOf()))) {
            std::cerr << "Failed to create D3D11 device." << std::endl;
            return false;
        }

        // Get DXGI device and create WinRT device
        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(g_d3dDevice.As(&dxgiDevice))) {
             std::cerr << "Failed to get DXGI device." << std::endl;
              return false;
         }
         // Use the specific interop function to create the WinRT device
         HRESULT hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), reinterpret_cast<IInspectable**>(winrt::put_abi(g_winrtDevice)));
         if (FAILED(hr)) {
              std::cerr << "Failed to create WinRT Direct3DDevice from DXGI device. HRESULT: 0x" << std::hex << hr << std::endl;
              return false;
         }

         if (!g_winrtDevice) {
             std::cerr << "Failed to create WinRT Direct3DDevice." << std::endl;
             return false;
        }
        return true;
    }


    // --- Capture Thread: Uses Windows Graphics Capture API ---
    static void CaptureThread() {
        // Initialize COM and WinRT for this thread - Use STA
        winrt::init_apartment(winrt::apartment_type::single_threaded);

        // Create the D3D device
        if (!CreateDirect3DDevice()) {
            g_running = false;
            winrt::uninit_apartment();
            return;
        }

        // --- Capture Primary Monitor Directly (Bypass Picker) ---
        winrt::GraphicsCaptureItem item{ nullptr };
        try {
             // Get the HMONITOR of the primary monitor.
             HMONITOR hmon = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
             if (!hmon) {
                  std::cerr << "Failed to get primary monitor handle." << std::endl;
                  g_running = false;
                  winrt::uninit_apartment();
                  return;
             }
             std::cout << "Attempting to capture primary monitor (HMONITOR: " << hmon << ")..." << std::endl;

             void* item_abi = nullptr;
             // Get the activation factory and query for the IGraphicsCaptureItemInterop interface
             auto interop_factory = winrt::get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
             // Call CreateForMonitor using the interop factory interface
             HRESULT hr_create = interop_factory->CreateForMonitor(hmon, winrt::guid_of<winrt::GraphicsCaptureItem>(), &item_abi);
             std::cout << "CreateForMonitor HRESULT: 0x" << std::hex << hr_create << std::endl; // Log HRESULT
             winrt::check_hresult(hr_create); // Check HRESULT after logging

             if (item_abi) {
                 // Construct directly from the ABI pointer, taking ownership
                 item = { static_cast<winrt::impl::abi<winrt::GraphicsCaptureItem>::type*>(item_abi), winrt::take_ownership_from_abi };
             } else {
                  // Should not happen if CreateForMonitor succeeds without error, but check anyway
                  std::cerr << "CreateForMonitor succeeded but returned null ABI pointer." << std::endl;
                  g_running = false;
                  winrt::uninit_apartment();
                  return;
             }

             // Item should be valid here if we didn't return
             if (!item) { // Double check just in case copy_from_abi resulted in null
                 std::cerr << "Failed to create GraphicsCaptureItem from ABI pointer." << std::endl;
                 g_running = false;
                 winrt::uninit_apartment();
                 return;
             }
        } catch (winrt::hresult_error const& ex) {
             // Use generic error message as picker is bypassed
             std::cerr << "Failed to create capture item for monitor: " << winrt::to_string(ex.message()) << std::endl;
             g_running = false;
             winrt::uninit_apartment();
             return;
        }


        // Create frame pool and session
        g_lastFrameSize = item.Size();
        g_captureWidth = g_lastFrameSize.Width;
        g_captureHeight = g_lastFrameSize.Height;
        std::cout << "Capture target selected (Primary Monitor). Size: " << g_captureWidth << "x" << g_captureHeight << std::endl;


        g_framePool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(
            g_winrtDevice,
            winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized, // Matches typical screen format
            2, // Number of buffers
            g_lastFrameSize);

        if (!g_framePool) {
             std::cerr << "Failed to create frame pool." << std::endl;
             g_running = false;
             winrt::uninit_apartment();
             return;
        }

        g_session = g_framePool.CreateCaptureSession(item);
        if (!g_session) {
             std::cerr << "Failed to create capture session." << std::endl;
             g_running = false;
             winrt::uninit_apartment();
             return;
        }

        // Prepare a staging texture (needs to be recreated if size changes)
        ComPtr<ID3D11Texture2D> stagingTex;
        D3D11_TEXTURE2D_DESC stagingDesc = {};
        stagingDesc.Width = g_captureWidth.load(); // Use atomic load
        stagingDesc.Height = g_captureHeight.load(); // Use atomic load
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;

        if (FAILED(g_d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &stagingTex))) {
            std::cerr << "Failed to create staging texture." << std::endl;
            g_running = false;
            winrt::uninit_apartment();
            return;
        }


        // --- Frame Arrived Event Handler ---
        // Store the token when registering the event handler
        g_frameArrivedToken = g_framePool.FrameArrived([&](winrt::Direct3D11CaptureFramePool const& sender, winrt::Windows::Foundation::IInspectable const& args) {
            bool newSize = false;
            winrt::Direct3D11CaptureFrame frame = nullptr;

            try {
                 frame = sender.TryGetNextFrame();
                 if (!frame) return; // Should not happen in free-threaded mode if called correctly

                 winrt::SizeInt32 currentSize = frame.ContentSize();
                 if (currentSize.Width != g_lastFrameSize.Width || currentSize.Height != g_lastFrameSize.Height) {
                     g_lastFrameSize = currentSize;
                     g_captureWidth = g_lastFrameSize.Width;
                     g_captureHeight = g_lastFrameSize.Height;
                     newSize = true;
                     std::cout << "Capture size changed: " << g_captureWidth << "x" << g_captureHeight << std::endl;
                     // Need to recreate frame pool and staging texture
                     // For simplicity here, we'll just stop - a real app should recreate
                     std::cerr << "Capture size changed, stopping capture for simplicity." << std::endl;
                     g_running = false; // Signal other threads to stop
                     // No need to close session/pool here, cleanup will handle it
                     return;
                     // TODO: Implement robust resize handling (recreate framepool, session, staging texture)
                 }

                 // Get frame surface
                 ComPtr<ID3D11Texture2D> frameSurface;
                 auto access = frame.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                 if (FAILED(access->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(frameSurface.ReleaseAndGetAddressOf())))) {
                      std::cerr << "Failed to get frame surface." << std::endl;
                      return;
                 }

                 // Copy to staging texture
                 g_d3dContext->CopyResource(stagingTex.Get(), frameSurface.Get());

                 // Map staging texture
                 D3D11_MAPPED_SUBRESOURCE mapped;
                 if (SUCCEEDED(g_d3dContext->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                     // --- Pass full BGRA frame ---
                     // Wrap the full captured frame in a cv::Mat.
                     cv::Mat fullImage(g_captureHeight.load(), g_captureWidth.load(), CV_8UC4, mapped.pData, mapped.RowPitch);

                     {
                         std::lock_guard<std::mutex> lock(g_frameMutex);
                         // Store the full BGRA frame directly
                         g_capturedFrame = fullImage.clone();
                         g_newFrame = true;
                     }
                     g_frameCond.notify_one();
                     // --- End Pass full BGRA frame ---

                     g_d3dContext->Unmap(stagingTex.Get(), 0);
                 } else {
                      std::cerr << "Failed to map staging texture." << std::endl;
                 }

            } catch (winrt::hresult_error const& error) {
                 std::cerr << "Frame processing error: " << winrt::to_string(error.message()) << std::endl;
                 // Consider stopping capture on persistent errors
            }
            // IMPORTANT: Close the frame to return buffer to pool
            if (frame) {
                frame.Close();
            }
        });

        // Start capturing
        try {
             g_session.StartCapture();
             std::cout << "Capture started. Press Q to exit." << std::endl;
        } catch (winrt::hresult_error const& ex) {
             std::cerr << "Failed to start capture: " << winrt::to_string(ex.message()) << std::endl;
             g_running = false;
        }


        // Keep thread alive while running
        while (g_running) {
            // Check for session closure or errors periodically
            if (g_session && !g_session.IsCursorCaptureEnabled()) { // Check if session exists before checking property
                 // Session might have been closed due to error or size change
                 // g_running should be false if we initiated stop
                 if (g_running) {
                      std::cerr << "Capture session seems inactive, stopping." << std::endl;
                      g_running = false;
                 }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // Cleanup
        std::cout << "Stopping capture thread..." << std::endl;
        // Flush context before cleanup
        if (g_d3dContext) {
             g_d3dContext->Flush();
        }
        // Revoke the event handler first to prevent calls during cleanup
        if (g_framePool && g_frameArrivedToken.value != 0) { // Check if token is valid
             try {
                 g_framePool.FrameArrived(g_frameArrivedToken);
             } catch (winrt::hresult_error const& ex) {
                 // Log error if revoke fails, but continue cleanup
                 std::cerr << "Error revoking FrameArrived handler: " << winrt::to_string(ex.message()) << std::endl;
             }
             g_frameArrivedToken.value = 0; // Mark token as invalid
        }
        // Explicitly release objects in a potentially safer order
        g_session = nullptr;    // Release session first
        g_framePool = nullptr;    // Then frame pool
        g_winrtDevice = nullptr; // Then WinRT device wrapper
        // Then release D3D objects (ComPtr handles this)
        g_d3dContext = nullptr;
        g_d3dDevice = nullptr;

        winrt::uninit_apartment(); // Uninit apartment last
        std::cout << "Capture thread finished." << std::endl;
    }

    // --- Processing Thread: CUDA-based target detection and update ---
    static void ProcessingThread() {
        // Create CUDA filters once outside the loop for efficiency
        cv::Ptr<cv::cuda::Filter> openingFilter = cv::cuda::createMorphologyFilter(cv::MORPH_OPEN, CV_8UC1, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(5, 5)));

        // GPU Mats for processing - Declare outside loop for reuse
        cv::cuda::GpuMat full_frame_gpu_bgra, full_frame_gpu_bgr, roi_gpu_bgr, hsv_gpu, mask_gpu;
        std::vector<cv::cuda::GpuMat> hsv_channels(3);
        cv::cuda::GpuMat h_mask_lower, h_mask_upper, h_mask;
        cv::cuda::GpuMat s_mask_lower, s_mask_upper, s_mask;
        cv::cuda::GpuMat v_mask_lower, v_mask_upper, v_mask;
        cv::cuda::GpuMat combined_mask1;

        while (g_running) {
            cv::Mat full_frame_bgra; // Stores the full BGRA frame from capture thread

            { // Lock scope
                 std::unique_lock<std::mutex> lock(g_frameMutex);
                 g_frameCond.wait(lock, [] { return g_newFrame.load() || !g_running; });
                 if (!g_running) break;
                 full_frame_bgra = g_capturedFrame.clone(); // Clone the full BGRA frame
                 g_newFrame = false;
            } // Unlock mutex


            if (full_frame_bgra.empty()) {
                continue;
            }

            // --- Optimization: Process full frame on GPU ---
            // Upload full BGRA frame to GPU
            full_frame_gpu_bgra.upload(full_frame_bgra);

            // Convert full frame BGRA -> BGR on GPU
            cv::cuda::cvtColor(full_frame_gpu_bgra, full_frame_gpu_bgr, cv::COLOR_BGRA2BGR);

            // Recalculate ROI based on current capture size (use atomics for safety)
            int currentCaptureWidth = g_captureWidth.load();
            int currentCaptureHeight = g_captureHeight.load();
            int currentRoiLeft = (currentCaptureWidth / 2) - (ROI_WIDTH / 2);
            int currentRoiTop = (currentCaptureHeight / 2) - (ROI_HEIGHT / 2);
            currentRoiLeft = std::max(0, std::min(currentRoiLeft, currentCaptureWidth - ROI_WIDTH));
            currentRoiTop = std::max(0, std::min(currentRoiTop, currentCaptureHeight - ROI_HEIGHT));
            int currentRoiWidth = std::min(ROI_WIDTH, currentCaptureWidth - currentRoiLeft);
            int currentRoiHeight = std::min(ROI_HEIGHT, currentCaptureHeight - currentRoiTop);

            // Update global ROI atomics (optional, but good practice)
            g_roiLeft = currentRoiLeft;
            g_roiTop = currentRoiTop;
            g_roiWidthActual = currentRoiWidth;
            g_roiHeightActual = currentRoiHeight;

            if (currentRoiWidth <= 0 || currentRoiHeight <= 0) {
                 std::cerr << "Calculated ROI has zero width or height in processing thread." << std::endl;
                 continue; // Skip frame if ROI is invalid
            }

            // Extract ROI on GPU
            cv::Rect roi_rect(currentRoiLeft, currentRoiTop, currentRoiWidth, currentRoiHeight);
            roi_gpu_bgr = full_frame_gpu_bgr(roi_rect);
            // --- End Optimization ---


            // Convert ROI BGR -> HSV on GPU
            cv::cuda::cvtColor(roi_gpu_bgr, hsv_gpu, cv::COLOR_BGR2HSV); // Use roi_gpu_bgr

            // --- Replace cv::cuda::inRange with thresholding and bitwise_and ---
            cv::Scalar lower_bound(20, 100, 100);
            cv::Scalar upper_bound(30, 255, 255);
            //std::vector<cv::cuda::GpuMat> hsv_channels(3); // Moved outside loop
            cv::cuda::split(hsv_gpu, hsv_channels); // Operate on hsv_gpu (ROI)
            // Moved GpuMat declarations outside loop
            cv::cuda::threshold(hsv_channels[0], h_mask_lower, lower_bound[0], 255.0, cv::THRESH_BINARY);
            cv::cuda::threshold(hsv_channels[0], h_mask_upper, upper_bound[0], 255.0, cv::THRESH_BINARY_INV);
            cv::cuda::bitwise_and(h_mask_lower, h_mask_upper, h_mask);
            cv::cuda::threshold(hsv_channels[1], s_mask_lower, lower_bound[1], 255.0, cv::THRESH_BINARY);
            cv::cuda::threshold(hsv_channels[1], s_mask_upper, upper_bound[1], 255.0, cv::THRESH_BINARY_INV);
            cv::cuda::bitwise_and(s_mask_lower, s_mask_upper, s_mask);
            cv::cuda::threshold(hsv_channels[2], v_mask_lower, lower_bound[2], 255.0, cv::THRESH_BINARY);
            cv::cuda::threshold(hsv_channels[2], v_mask_upper, upper_bound[2], 255.0, cv::THRESH_BINARY_INV);
            cv::cuda::bitwise_and(v_mask_lower, v_mask_upper, v_mask);
            cv::cuda::bitwise_and(h_mask, s_mask, combined_mask1);
            cv::cuda::bitwise_and(combined_mask1, v_mask, mask_gpu); // Final mask_gpu is for the ROI
            // --- End replacement ---

            // Morphological Opening on GPU (on the ROI mask)
            openingFilter->apply(mask_gpu, mask_gpu);
            // Removed separate erode/dilate

            // Download the processed mask back to CPU for contour finding
            cv::Mat mask_cpu;
            mask_gpu.download(mask_cpu);

            // Find contours on CPU using the downloaded mask.
            std::vector<std::vector<cv::Point>> contours;
            cv::findContours(mask_cpu, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

            // --- Filter contours by area ---
            std::vector<std::vector<cv::Point>> filtered_contours;
            for (const auto& contour : contours) {
                double area = cv::contourArea(contour);
                if (area >= MIN_CONTOUR_AREA) {
                    filtered_contours.push_back(contour);
                }
            }
            // --- End filtering ---


            cv::Point closestPoint(-1, -1);
            double minDistance = std::numeric_limits<double>::max();
            bool targetFoundThisFrame = false;

            // --- Calculate crosshair relative to the ROI ---
            int crosshairInRoiX = (currentCaptureWidth / 2) - currentRoiLeft;
            int crosshairInRoiY = (currentCaptureHeight / 2) - currentRoiTop;
            cv::Point crosshairInRoi(crosshairInRoiX, crosshairInRoiY);
            // --- End crosshair calculation ---

            // --- Target Sticking Logic ---
            cv::Point potentialTarget(-1,-1);
            // 1. Check if previous target is still valid and nearby
            if (g_previousTargetPoint.x != -1) {
                for (const auto& contour : filtered_contours) {
                    cv::Moments M = cv::moments(contour);
                    if (M.m00 != 0) {
                        int cX = static_cast<int>(std::round(M.m10 / M.m00));
                        int cY = static_cast<int>(std::round(M.m01 / M.m00));
                        cv::Point currentCentroid(cX, cY);
                        if (calculateDistance(g_previousTargetPoint, currentCentroid) < MAX_TARGET_STICK_DISTANCE) {
                            potentialTarget = currentCentroid; // Found previous target nearby
                            minDistance = calculateDistance(crosshairInRoi, potentialTarget); // Use its distance
                            targetFoundThisFrame = true;
                            break; // Prioritize sticking to the previous target
                        }
                    }
                }
            }

            // 2. If previous target wasn't found nearby, find the contour closest to the crosshair
            if (!targetFoundThisFrame) {
                 minDistance = std::numeric_limits<double>::max(); // Reset minDistance
                 for (const auto& contour : filtered_contours) {
                     cv::Moments M = cv::moments(contour);
                     if (M.m00 != 0) {
                         int cX = static_cast<int>(std::round(M.m10 / M.m00));
                         int cY = static_cast<int>(std::round(M.m01 / M.m00));
                         cv::Point pt(cX, cY); // Point is relative to ROI top-left
                         double dist = calculateDistance(crosshairInRoi, pt);
                         if (dist < minDistance) {
                             minDistance = dist;
                             potentialTarget = pt; // Store point relative to ROI
                             targetFoundThisFrame = true;
                         }
                     }
                 }
            }
            // --- End Target Sticking Logic ---

            closestPoint = potentialTarget; // Assign the determined target

            // Update previous target point for next frame
            g_previousTargetPoint = closestPoint;


            // If a target is detected and aimbot is active, update global shooting coordinates.
            if (closestPoint.x != -1 && closestPoint.y != -1 && g_aimbotActive) {
                // Convert closestPoint (relative to ROI) to coordinates relative to full captured frame
                cv::Point targetInCapture(closestPoint.x + currentRoiLeft, closestPoint.y + currentRoiTop);

                // Calculate relative movement from the center of the CAPTURED frame
                int crosshairInCaptureX = currentCaptureWidth / 2;
                int crosshairInCaptureY = currentCaptureHeight / 2;
                int relX = targetInCapture.x - crosshairInCaptureX;
                int relY = targetInCapture.y - crosshairInCaptureY;

                // Apply smoothing
                int moveX = static_cast<int>(std::round(relX * AIM_SMOOTHING_FACTOR));
                int moveY = static_cast<int>(std::round(relY * AIM_SMOOTHING_FACTOR));

                // Ensure minimum movement
                if (std::abs(relX) > MOVEMENT_THRESHOLD && moveX == 0) moveX = (relX > 0) ? 1 : -1;
                if (std::abs(relY) > MOVEMENT_THRESHOLD && moveY == 0) moveY = (relY > 0) ? 1 : -1;

                if (moveX != 0 || moveY != 0) {
                    simulateMouseMoveRelative(moveX, moveY);
                }

                // Update global target coordinates (relative to captured item top-left)
                g_targetX.store(targetInCapture.x);
                g_targetY.store(targetInCapture.y);
                g_targetDetected.store(true);
            }
            else {
                g_targetDetected.store(false);
                g_previousTargetPoint = cv::Point(-1, -1); // Reset previous target if none found
            }
        }
        std::cout << "Processing thread finished." << std::endl;
    }

    // --- Shooting Thread: Rapidly clicks the mouse while target is detected ---
    static void ShootingThread() {
        while (g_running) {
            if (g_aimbotActive && g_targetDetected.load()) {
                int x = g_targetX.load(); // Coordinates are relative to captured item top-left
                int y = g_targetY.load();
                // To click accurately, we'd need the screen coordinates of the captured item's top-left corner.
                // For now, we assume it's (0,0) or the user handles positioning.
                // A more complex solution would use GetWindowRect if a window is captured.
                simulateMouseClick(x, y); // Click relative to captured item for now
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            else {
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
        }
         std::cout << "Shooting thread finished." << std::endl;
    }

} // end anonymous namespace

int main() {
    // Initialize WinRT for the main thread (MTA by default)
    winrt::init_apartment();

    // Check for CUDA device
    if (cv::cuda::getCudaEnabledDeviceCount() == 0) {
        std::cerr << "Error: No CUDA-enabled device found. Exiting." << std::endl;
        return -1;
    }
    std::cout << "CUDA device found. Initializing..." << std::endl;
    cv::cuda::printShortCudaDeviceInfo(cv::cuda::getDevice());

    // Message changed as picker is bypassed
    std::cout << "Initializing capture for primary monitor..." << std::endl;

    std::thread capThread(CaptureThread);
    // Give capture thread a moment to initialize
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Check if capture initialization failed early
    if (!g_running) {
         std::cerr << "Capture initialization failed. Exiting." << std::endl;
         if (capThread.joinable()) capThread.join();
         return -1;
    }

    std::thread procThread(ProcessingThread);
    std::thread shootThread(ShootingThread);

    std::cout << "Press F8 to toggle aimbot ON/OFF, Q to exit." << std::endl;

    bool togglePressed = false;
    while (g_running) {
        if (GetAsyncKeyState(VK_KEY_Q) & 0x8000) {
            g_running = false;
            g_frameCond.notify_all(); // Wake up processing thread if waiting
            break;
        }
        if (GetAsyncKeyState(VK_KEY_F8) & 0x8000) {
            if (!togglePressed) {
                g_aimbotActive = !g_aimbotActive;
                togglePressed = true;
                std::cout << "Aimbot " << (g_aimbotActive ? "ON" : "OFF") << std::endl;
            }
        }
        else {
            togglePressed = false;
        }
        // Check if capture thread signaled stop
        if (!g_running) {
             g_frameCond.notify_all();
             break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Main loop doesn't need to spin super fast
    }

    std::cout << "Main loop exiting. Waiting for threads..." << std::endl;
    if (capThread.joinable()) capThread.join();
    if (procThread.joinable()) procThread.join();
    if (shootThread.joinable()) shootThread.join();

    std::cout << "All threads finished. Exiting." << std::endl;
    return 0;
}
