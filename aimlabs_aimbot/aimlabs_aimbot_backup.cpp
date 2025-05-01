#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <opencv2/opencv.hpp>
#include <opencv2/imgproc.hpp>
#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cmath>
#include <vector>

// Link necessary libraries
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

using Microsoft::WRL::ComPtr;

namespace {

    // --- Constants ---
    const int SCREEN_WIDTH = 1920;
    const int SCREEN_HEIGHT = 1080;
    const int CROSSHAIR_X = SCREEN_WIDTH / 2;
    const int CROSSHAIR_Y = SCREEN_HEIGHT / 2;

    // Increased ROI: now 800x600 (centered on the crosshair)
    const int ROI_WIDTH = 800;
    const int ROI_HEIGHT = 600;
    const int ROI_LEFT = CROSSHAIR_X - (ROI_WIDTH / 2);
    const int ROI_TOP = CROSSHAIR_Y - (ROI_HEIGHT / 2);

    const int MOVEMENT_THRESHOLD = 5; // Minimal movement threshold

    // Virtual-Key codes for control
    const int VK_KEY_Q = 0x51;  // Q to exit
    const int VK_KEY_F8 = 0x77;  // F8 to toggle aimbot on/off

    // --- Global Variables for Thread Coordination ---
    std::mutex g_frameMutex;
    std::condition_variable g_frameCond;
    cv::Mat g_roiFrame;
    std::atomic<bool> g_newFrame(false);
    std::atomic<bool> g_running(true);
    std::atomic<bool> g_aimbotActive(false);

    // Global shooting variables (target full-screen coordinates)
    std::atomic<bool> g_targetDetected(false);
    std::atomic<int> g_targetX(0);
    std::atomic<int> g_targetY(0);

    // --- Utility Functions ---
    static double calculateDistance(const cv::Point& p1, const cv::Point& p2) {
        return std::hypot(p1.x - p2.x, p1.y - p2.y);
    }

    static void simulateMouseClick(int /*x*/, int /*y*/) {
        // Simulate a left mouse click.
        INPUT inputs[2] = {};
        ZeroMemory(inputs, sizeof(inputs));
        inputs[0].type = INPUT_MOUSE;
        inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
        inputs[1].type = INPUT_MOUSE;
        inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
        SendInput(2, inputs, sizeof(INPUT));
    }

    static void simulateMouseMoveRelative(int dx, int dy) {
        // Simulate relative mouse movement.
        INPUT input = {};
        ZeroMemory(&input, sizeof(input));
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        input.mi.dx = dx;
        input.mi.dy = dy;
        SendInput(1, &input, sizeof(INPUT));
    }

    // --- Capture Thread: Uses DXGI Desktop Duplication API ---
    static void CaptureThread() {
        // Create D3D11 device and context.
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
        ComPtr<ID3D11Device> d3dDevice;
        ComPtr<ID3D11DeviceContext> d3dContext;
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            &featureLevel, 1, D3D11_SDK_VERSION, &d3dDevice, nullptr, &d3dContext))) {
            std::cerr << "Failed to create D3D11 device." << std::endl;
            g_running = false;
            return;
        }

        // Get DXGI device.
        ComPtr<IDXGIDevice> dxgiDevice;
        d3dDevice.As(&dxgiDevice);
        if (!dxgiDevice) {
            std::cerr << "Failed to get DXGI device." << std::endl;
            g_running = false;
            return;
        }

        // Get DXGI adapter.
        ComPtr<IDXGIAdapter> dxgiAdapter;
        if (FAILED(dxgiDevice->GetParent(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(dxgiAdapter.GetAddressOf())))) {
            std::cerr << "Failed to get DXGI adapter." << std::endl;
            g_running = false;
            return;
        }

        // Get output (monitor) – we use the first output.
        ComPtr<IDXGIOutput> dxgiOutput;
        if (FAILED(dxgiAdapter->EnumOutputs(0, &dxgiOutput))) {
            std::cerr << "Failed to get DXGI output." << std::endl;
            g_running = false;
            return;
        }

        // Query for IDXGIOutput1.
        ComPtr<IDXGIOutput1> dxgiOutput1;
        dxgiOutput.As(&dxgiOutput1);
        if (!dxgiOutput1) {
            std::cerr << "Failed to get IDXGIOutput1." << std::endl;
            g_running = false;
            return;
        }

        // Create the duplication interface.
        ComPtr<IDXGIOutputDuplication> deskDupl;
        if (FAILED(dxgiOutput1->DuplicateOutput(d3dDevice.Get(), &deskDupl))) {
            std::cerr << "Failed to duplicate output." << std::endl;
            g_running = false;
            return;
        }

        // Prepare a staging texture for frame copy.
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = SCREEN_WIDTH;
        desc.Height = SCREEN_HEIGHT;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;

        ComPtr<ID3D11Texture2D> stagingTex;
        if (FAILED(d3dDevice->CreateTexture2D(&desc, nullptr, &stagingTex))) {
            std::cerr << "Failed to create staging texture." << std::endl;
            g_running = false;
            return;
        }

        while (g_running) {
            ComPtr<IDXGIResource> desktopResource;
            DXGI_OUTDUPL_FRAME_INFO frameInfo;
            HRESULT hr = deskDupl->AcquireNextFrame(16, &frameInfo, &desktopResource);
            if (FAILED(hr)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            ComPtr<ID3D11Texture2D> acquiredTex;
            desktopResource.As(&acquiredTex);
            if (acquiredTex) {
                d3dContext->CopyResource(stagingTex.Get(), acquiredTex.Get());
                D3D11_MAPPED_SUBRESOURCE mapped;
                if (SUCCEEDED(d3dContext->Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                    // Wrap the full-screen frame in a cv::Mat.
                    cv::Mat fullImage(SCREEN_HEIGHT, SCREEN_WIDTH, CV_8UC4, mapped.pData, mapped.RowPitch);
                    // Crop the ROI from the full image.
                    cv::Mat roiImage = fullImage(cv::Rect(ROI_LEFT, ROI_TOP, ROI_WIDTH, ROI_HEIGHT)).clone();
                    {
                        std::lock_guard<std::mutex> lock(g_frameMutex);
                        g_roiFrame = roiImage;
                        g_newFrame = true;
                    }
                    g_frameCond.notify_one();
                    d3dContext->Unmap(stagingTex.Get(), 0);
                }
            }
            deskDupl->ReleaseFrame();
        }
    }

    // --- Processing Thread: CPU-based target detection and update ---
    static void ProcessingThread() {
        while (g_running) {
            std::unique_lock<std::mutex> lock(g_frameMutex);
            g_frameCond.wait(lock, [] { return g_newFrame.load() || !g_running; });
            if (!g_running) break;
            cv::Mat frame = g_roiFrame.clone();
            g_newFrame = false;
            lock.unlock();

            if (frame.empty())
                continue;

            // Convert to HSV and threshold for teal/turquoise.
            cv::Mat hsv, mask;
            cv::cvtColor(frame, hsv, cv::COLOR_BGR2HSV);
            cv::inRange(hsv, cv::Scalar(75, 100, 100), cv::Scalar(105, 255, 255), mask);

            // Find contours.
            std::vector<std::vector<cv::Point>> contours;
            cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

            cv::Point closestPoint(-1, -1);
            double minDistance = std::numeric_limits<double>::max();
            cv::Point roiCenter(ROI_WIDTH / 2, ROI_HEIGHT / 2);

            for (const auto& contour : contours) {
                cv::Moments M = cv::moments(contour);
                if (M.m00 != 0) {
                    int cX = static_cast<int>(std::round(M.m10 / M.m00));
                    int cY = static_cast<int>(std::round(M.m01 / M.m00));
                    cv::Point pt(cX, cY);
                    double dist = calculateDistance(roiCenter, pt);
                    if (dist < minDistance) {
                        minDistance = dist;
                        closestPoint = pt;
                    }
                }
            }

            // If a target is detected and aimbot is active, update global shooting coordinates.
            if (closestPoint.x != -1 && closestPoint.y != -1 && g_aimbotActive) {
                cv::Point targetFull(closestPoint.x + ROI_LEFT, closestPoint.y + ROI_TOP);
                // Optionally, adjust the mouse position if needed.
                int relX = targetFull.x - CROSSHAIR_X;
                int relY = targetFull.y - CROSSHAIR_Y;
                if (std::abs(relX) > MOVEMENT_THRESHOLD || std::abs(relY) > MOVEMENT_THRESHOLD) {
                    int moveX = static_cast<int>(std::round(relX));
                    int moveY = static_cast<int>(std::round(relY));
                    simulateMouseMoveRelative(moveX, moveY);
                }
                // Update global target coordinates for the shooting thread.
                g_targetX.store(targetFull.x);
                g_targetY.store(targetFull.y);
                g_targetDetected.store(true);
            }
            else {
                g_targetDetected.store(false);
            }
        }
    }

    // --- Shooting Thread: Rapidly clicks the mouse while target is detected ---
    static void ShootingThread() {
        while (g_running) {
            if (g_aimbotActive && g_targetDetected.load()) {
                int x = g_targetX.load();
                int y = g_targetY.load();
                simulateMouseClick(x, y);
                // Very short delay for rapid clicking.
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            else {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }

} // end anonymous namespace

int main() {
    std::cout << "Press F8 to toggle aimbot ON/OFF, Q to exit." << std::endl;

    std::thread capThread(CaptureThread);
    std::thread procThread(ProcessingThread);
    std::thread shootThread(ShootingThread);

    bool togglePressed = false;
    while (g_running) {
        if (GetAsyncKeyState(VK_KEY_Q) & 0x8000) {
            g_running = false;
            g_frameCond.notify_all();
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
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    capThread.join();
    procThread.join();
    shootThread.join();
    return 0;
}
