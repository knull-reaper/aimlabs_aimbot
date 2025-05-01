#include <windows.h>

#include <d3d11.h>

#include <dxgi1_2.h>

#include <wrl/client.h>


#pragma warning(push, 0)#include <opencv2/opencv.hpp>

#include <opencv2/imgproc.hpp>

#include <opencv2/cudaimgproc.hpp>

#include <opencv2/cudafilters.hpp>

#include <opencv2/cudaarithm.hpp>

#pragma warning(pop)

#include <winrt/Windows.Foundation.h>

#include <winrt/Windows.Graphics.Capture.h>

#include <winrt/Windows.Graphics.DirectX.h>

#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <windows.graphics.capture.interop.h>

#include <windows.graphics.directx.direct3d11.interop.h>

#include <iostream>

#include <thread>

#include <atomic>

#include <mutex>

#include <condition_variable>

#include <chrono>

#include <cmath>

#include <vector>

#include <algorithm>

#include <dwmapi.h>


#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

using Microsoft::WRL::ComPtr;
namespace winrt {
  using namespace Windows::Foundation;
  using namespace Windows::Graphics;
  using namespace Windows::Graphics::Capture;
  using namespace Windows::Graphics::DirectX;
  using namespace Windows::Graphics::DirectX::Direct3D11;
}

namespace {

  const int SCREEN_WIDTH = 1920;
  const int SCREEN_HEIGHT = 1080;
  const int CROSSHAIR_X = SCREEN_WIDTH / 2;
  const int CROSSHAIR_Y = SCREEN_HEIGHT / 2;

  const int ROI_WIDTH = 1024;
  const int ROI_HEIGHT = 768;

  std::atomic < int > g_roiLeft = CROSSHAIR_X - (ROI_WIDTH / 2);
  std::atomic < int > g_roiTop = CROSSHAIR_Y - (ROI_HEIGHT / 2);
  std::atomic < int > g_roiWidthActual = ROI_WIDTH;
  std::atomic < int > g_roiHeightActual = ROI_HEIGHT;

  std::atomic < int > g_captureWidth = SCREEN_WIDTH;
  std::atomic < int > g_captureHeight = SCREEN_HEIGHT;

  const int MOVEMENT_THRESHOLD = 1;
  const double AIM_SMOOTHING_FACTOR = 1.0;
  const double MIN_CONTOUR_AREA = 50.0;
  const double MAX_TARGET_STICK_DISTANCE = 15.0;

  const int VK_KEY_Q = 0x51;
  const int VK_KEY_F8 = 0x77;

  std::mutex g_frameMutex;
  std::condition_variable g_frameCond;
  cv::Mat g_capturedFrame;
  std::atomic < bool > g_newFrame(false);
  std::atomic < bool > g_running(true);
  std::atomic < bool > g_aimbotActive(false);

  std::atomic < bool > g_targetDetected(false);
  std::atomic < int > g_targetX(0);
  std::atomic < int > g_targetY(0);
  cv::Point g_previousTargetPoint(-1, -1);

  winrt::IDirect3DDevice g_winrtDevice {
    nullptr
  };
  ComPtr < ID3D11Device > g_d3dDevice;
  ComPtr < ID3D11DeviceContext > g_d3dContext;
  winrt::Direct3D11CaptureFramePool g_framePool {
    nullptr
  };
  winrt::GraphicsCaptureSession g_session {
    nullptr
  };
  winrt::SizeInt32 g_lastFrameSize;
  winrt::event_token g_frameArrivedToken;

  static double calculateDistance(const cv::Point & p1,
    const cv::Point & p2) {
    return std::hypot(p1.x - p2.x, p1.y - p2.y);
  }

  static void simulateMouseClick(int /*x*/ , int /*y*/ ) {
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
    ZeroMemory( & input, sizeof(input));
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    SendInput(1, & input, sizeof(INPUT));
  }

  bool CreateDirect3DDevice() {
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    #ifdef _DEBUG

    #endif

    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, &
        featureLevel, 1, D3D11_SDK_VERSION, g_d3dDevice.ReleaseAndGetAddressOf(), nullptr, g_d3dContext.ReleaseAndGetAddressOf()))) {
      std::cerr << "Failed to create D3D11 device." << std::endl;
      return false;
    }

    ComPtr < IDXGIDevice > dxgiDevice;
    if (FAILED(g_d3dDevice.As( & dxgiDevice))) {
      std::cerr << "Failed to get DXGI device." << std::endl;
      return false;
    }

    HRESULT hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), reinterpret_cast < IInspectable ** > (winrt::put_abi(g_winrtDevice)));
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

  static void CaptureThread() {

    winrt::init_apartment(winrt::apartment_type::single_threaded);

    if (!CreateDirect3DDevice()) {
      g_running = false;
      winrt::uninit_apartment();
      return;
    }

    winrt::GraphicsCaptureItem item {
      nullptr
    };
    try {

      HMONITOR hmon = MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
      if (!hmon) {
        std::cerr << "Failed to get primary monitor handle." << std::endl;
        g_running = false;
        winrt::uninit_apartment();
        return;
      }
      std::cout << "Attempting to capture primary monitor (HMONITOR: " << hmon << ")..." << std::endl;

      void * item_abi = nullptr;

      auto interop_factory = winrt::get_activation_factory < winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop > ();

      HRESULT hr_create = interop_factory -> CreateForMonitor(hmon, winrt::guid_of < winrt::GraphicsCaptureItem > (), & item_abi);
      std::cout << "CreateForMonitor HRESULT: 0x" << std::hex << hr_create << std::endl;
      winrt::check_hresult(hr_create);

      if (item_abi) {

        item = {
          static_cast < winrt::impl::abi < winrt::GraphicsCaptureItem > ::type * > (item_abi),
          winrt::take_ownership_from_abi
        };
      } else {

        std::cerr << "CreateForMonitor succeeded but returned null ABI pointer." << std::endl;
        g_running = false;
        winrt::uninit_apartment();
        return;
      }

      if (!item) {
        std::cerr << "Failed to create GraphicsCaptureItem from ABI pointer." << std::endl;
        g_running = false;
        winrt::uninit_apartment();
        return;
      }
    } catch (winrt::hresult_error
      const & ex) {

      std::cerr << "Failed to create capture item for monitor: " << winrt::to_string(ex.message()) << std::endl;
      g_running = false;
      winrt::uninit_apartment();
      return;
    }

    g_lastFrameSize = item.Size();
    g_captureWidth = g_lastFrameSize.Width;
    g_captureHeight = g_lastFrameSize.Height;
    std::cout << "Capture target selected (Primary Monitor). Size: " << g_captureWidth << "x" << g_captureHeight << std::endl;

    g_framePool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(
      g_winrtDevice,
      winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized,
      2,
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

    ComPtr < ID3D11Texture2D > stagingTex;
    D3D11_TEXTURE2D_DESC stagingDesc = {};
    stagingDesc.Width = g_captureWidth.load();
    stagingDesc.Height = g_captureHeight.load();
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    if (FAILED(g_d3dDevice -> CreateTexture2D( & stagingDesc, nullptr, & stagingTex))) {
      std::cerr << "Failed to create staging texture." << std::endl;
      g_running = false;
      winrt::uninit_apartment();
      return;
    }

    g_frameArrivedToken = g_framePool.FrameArrived([ & ](winrt::Direct3D11CaptureFramePool
      const & sender, winrt::Windows::Foundation::IInspectable
      const & args) {
      bool newSize = false;
      winrt::Direct3D11CaptureFrame frame = nullptr;

      try {
        frame = sender.TryGetNextFrame();
        if (!frame) return;

        winrt::SizeInt32 currentSize = frame.ContentSize();
        if (currentSize.Width != g_lastFrameSize.Width || currentSize.Height != g_lastFrameSize.Height) {
          g_lastFrameSize = currentSize;
          g_captureWidth = g_lastFrameSize.Width;
          g_captureHeight = g_lastFrameSize.Height;
          newSize = true;
          std::cout << "Capture size changed: " << g_captureWidth << "x" << g_captureHeight << std::endl;

          std::cerr << "Capture size changed, stopping capture for simplicity." << std::endl;
          g_running = false;

          return;

        }

        ComPtr < ID3D11Texture2D > frameSurface;
        auto access = frame.Surface().as < Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess > ();
        if (FAILED(access -> GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast < void ** > (frameSurface.ReleaseAndGetAddressOf())))) {
          std::cerr << "Failed to get frame surface." << std::endl;
          return;
        }

        g_d3dContext -> CopyResource(stagingTex.Get(), frameSurface.Get());

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(g_d3dContext -> Map(stagingTex.Get(), 0, D3D11_MAP_READ, 0, & mapped))) {

          cv::Mat fullImage(g_captureHeight.load(), g_captureWidth.load(), CV_8UC4, mapped.pData, mapped.RowPitch);

          {
            std::lock_guard < std::mutex > lock(g_frameMutex);

            g_capturedFrame = fullImage.clone();
            g_newFrame = true;
          }
          g_frameCond.notify_one();

          g_d3dContext -> Unmap(stagingTex.Get(), 0);
        } else {
          std::cerr << "Failed to map staging texture." << std::endl;
        }

      } catch (winrt::hresult_error
        const & error) {
        std::cerr << "Frame processing error: " << winrt::to_string(error.message()) << std::endl;

      }

      if (frame) {
        frame.Close();
      }
    });

    try {
      g_session.StartCapture();
      std::cout << "Capture started. Press Q to exit." << std::endl;
    } catch (winrt::hresult_error
      const & ex) {
      std::cerr << "Failed to start capture: " << winrt::to_string(ex.message()) << std::endl;
      g_running = false;
    }

    while (g_running) {

      if (g_session && !g_session.IsCursorCaptureEnabled()) {

        if (g_running) {
          std::cerr << "Capture session seems inactive, stopping." << std::endl;
          g_running = false;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::cout << "Stopping capture thread..." << std::endl;

    if (g_d3dContext) {
      g_d3dContext -> Flush();
    }

    if (g_framePool && g_frameArrivedToken.value != 0) {
      try {
        g_framePool.FrameArrived(g_frameArrivedToken);
      } catch (winrt::hresult_error
        const & ex) {

        std::cerr << "Error revoking FrameArrived handler: " << winrt::to_string(ex.message()) << std::endl;
      }
      g_frameArrivedToken.value = 0;
    }

    g_session = nullptr;
    g_framePool = nullptr;
    g_winrtDevice = nullptr;

    g_d3dContext = nullptr;
    g_d3dDevice = nullptr;

    winrt::uninit_apartment();
    std::cout << "Capture thread finished." << std::endl;
  }

  static void ProcessingThread() {

    cv::Ptr < cv::cuda::Filter > openingFilter = cv::cuda::createMorphologyFilter(cv::MORPH_OPEN, CV_8UC1, cv::getStructuringElement(cv::MORPH_RECT, cv::Size(5, 5)));

    cv::cuda::GpuMat full_frame_gpu_bgra, full_frame_gpu_bgr, roi_gpu_bgr, hsv_gpu, mask_gpu;
    std::vector < cv::cuda::GpuMat > hsv_channels(3);
    cv::cuda::GpuMat h_mask_lower, h_mask_upper, h_mask;
    cv::cuda::GpuMat s_mask_lower, s_mask_upper, s_mask;
    cv::cuda::GpuMat v_mask_lower, v_mask_upper, v_mask;
    cv::cuda::GpuMat combined_mask1;

    while (g_running) {
      cv::Mat full_frame_bgra;

      {
        std::unique_lock < std::mutex > lock(g_frameMutex);
        g_frameCond.wait(lock, [] {
          return g_newFrame.load() || !g_running;
        });
        if (!g_running) break;
        full_frame_bgra = g_capturedFrame.clone();
        g_newFrame = false;
      }

      if (full_frame_bgra.empty()) {
        continue;
      }

      full_frame_gpu_bgra.upload(full_frame_bgra);

      cv::cuda::cvtColor(full_frame_gpu_bgra, full_frame_gpu_bgr, cv::COLOR_BGRA2BGR);

      int currentCaptureWidth = g_captureWidth.load();
      int currentCaptureHeight = g_captureHeight.load();
      int currentRoiLeft = (currentCaptureWidth / 2) - (ROI_WIDTH / 2);
      int currentRoiTop = (currentCaptureHeight / 2) - (ROI_HEIGHT / 2);
      currentRoiLeft = std::max(0, std::min(currentRoiLeft, currentCaptureWidth - ROI_WIDTH));
      currentRoiTop = std::max(0, std::min(currentRoiTop, currentCaptureHeight - ROI_HEIGHT));
      int currentRoiWidth = std::min(ROI_WIDTH, currentCaptureWidth - currentRoiLeft);
      int currentRoiHeight = std::min(ROI_HEIGHT, currentCaptureHeight - currentRoiTop);

      g_roiLeft = currentRoiLeft;
      g_roiTop = currentRoiTop;
      g_roiWidthActual = currentRoiWidth;
      g_roiHeightActual = currentRoiHeight;

      if (currentRoiWidth <= 0 || currentRoiHeight <= 0) {
        std::cerr << "Calculated ROI has zero width or height in processing thread." << std::endl;
        continue;
      }

      cv::Rect roi_rect(currentRoiLeft, currentRoiTop, currentRoiWidth, currentRoiHeight);
      roi_gpu_bgr = full_frame_gpu_bgr(roi_rect);

      cv::cuda::cvtColor(roi_gpu_bgr, hsv_gpu, cv::COLOR_BGR2HSV);

      cv::Scalar lower_bound(20, 100, 100);
      cv::Scalar upper_bound(30, 255, 255);

      cv::cuda::split(hsv_gpu, hsv_channels);

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
      cv::cuda::bitwise_and(combined_mask1, v_mask, mask_gpu);

      openingFilter -> apply(mask_gpu, mask_gpu);

      cv::Mat mask_cpu;
      mask_gpu.download(mask_cpu);

      std::vector < std::vector < cv::Point >> contours;
      cv::findContours(mask_cpu, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

      std::vector < std::vector < cv::Point >> filtered_contours;
      for (const auto & contour: contours) {
        double area = cv::contourArea(contour);
        if (area >= MIN_CONTOUR_AREA) {
          filtered_contours.push_back(contour);
        }
      }

      cv::Point closestPoint(-1, -1);
      double minDistance = std::numeric_limits < double > ::max();
      bool targetFoundThisFrame = false;

      int crosshairInRoiX = (currentCaptureWidth / 2) - currentRoiLeft;
      int crosshairInRoiY = (currentCaptureHeight / 2) - currentRoiTop;
      cv::Point crosshairInRoi(crosshairInRoiX, crosshairInRoiY);

      cv::Point potentialTarget(-1, -1);

      if (g_previousTargetPoint.x != -1) {
        for (const auto & contour: filtered_contours) {
          cv::Moments M = cv::moments(contour);
          if (M.m00 != 0) {
            int cX = static_cast < int > (std::round(M.m10 / M.m00));
            int cY = static_cast < int > (std::round(M.m01 / M.m00));
            cv::Point currentCentroid(cX, cY);
            if (calculateDistance(g_previousTargetPoint, currentCentroid) < MAX_TARGET_STICK_DISTANCE) {
              potentialTarget = currentCentroid;
              minDistance = calculateDistance(crosshairInRoi, potentialTarget);
              targetFoundThisFrame = true;
              break;
            }
          }
        }
      }

      if (!targetFoundThisFrame) {
        minDistance = std::numeric_limits < double > ::max();
        for (const auto & contour: filtered_contours) {
          cv::Moments M = cv::moments(contour);
          if (M.m00 != 0) {
            int cX = static_cast < int > (std::round(M.m10 / M.m00));
            int cY = static_cast < int > (std::round(M.m01 / M.m00));
            cv::Point pt(cX, cY);
            double dist = calculateDistance(crosshairInRoi, pt);
            if (dist < minDistance) {
              minDistance = dist;
              potentialTarget = pt;
              targetFoundThisFrame = true;
            }
          }
        }
      }

      closestPoint = potentialTarget;

      g_previousTargetPoint = closestPoint;

      if (closestPoint.x != -1 && closestPoint.y != -1 && g_aimbotActive) {

        cv::Point targetInCapture(closestPoint.x + currentRoiLeft, closestPoint.y + currentRoiTop);

        int crosshairInCaptureX = currentCaptureWidth / 2;
        int crosshairInCaptureY = currentCaptureHeight / 2;
        int relX = targetInCapture.x - crosshairInCaptureX;
        int relY = targetInCapture.y - crosshairInCaptureY;

        int moveX = static_cast < int > (std::round(relX * AIM_SMOOTHING_FACTOR));
        int moveY = static_cast < int > (std::round(relY * AIM_SMOOTHING_FACTOR));

        if (std::abs(relX) > MOVEMENT_THRESHOLD && moveX == 0) moveX = (relX > 0) ? 1 : -1;
        if (std::abs(relY) > MOVEMENT_THRESHOLD && moveY == 0) moveY = (relY > 0) ? 1 : -1;

        if (moveX != 0 || moveY != 0) {
          simulateMouseMoveRelative(moveX, moveY);
        }

        g_targetX.store(targetInCapture.x);
        g_targetY.store(targetInCapture.y);
        g_targetDetected.store(true);
      } else {
        g_targetDetected.store(false);
        g_previousTargetPoint = cv::Point(-1, -1);
      }
    }
    std::cout << "Processing thread finished." << std::endl;
  }

  static void ShootingThread() {
    while (g_running) {
      if (g_aimbotActive && g_targetDetected.load()) {
        int x = g_targetX.load();
        int y = g_targetY.load();

        simulateMouseClick(x, y);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
      }
    }
    std::cout << "Shooting thread finished." << std::endl;
  }

}

int main() {

  winrt::init_apartment();

  if (cv::cuda::getCudaEnabledDeviceCount() == 0) {
    std::cerr << "Error: No CUDA-enabled device found. Exiting." << std::endl;
    return -1;
  }
  std::cout << "CUDA device found. Initializing..." << std::endl;
  cv::cuda::printShortCudaDeviceInfo(cv::cuda::getDevice());

  std::cout << "Initializing capture for primary monitor..." << std::endl;

  std::thread capThread(CaptureThread);

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

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
      g_frameCond.notify_all();
      break;
    }
    if (GetAsyncKeyState(VK_KEY_F8) & 0x8000) {
      if (!togglePressed) {
        g_aimbotActive = !g_aimbotActive;
        togglePressed = true;
        std::cout << "Aimbot " << (g_aimbotActive ? "ON" : "OFF") << std::endl;
      }
    } else {
      togglePressed = false;
    }

    if (!g_running) {
      g_frameCond.notify_all();
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  std::cout << "Main loop exiting. Waiting for threads..." << std::endl;
  if (capThread.joinable()) capThread.join();
  if (procThread.joinable()) procThread.join();
  if (shootThread.joinable()) shootThread.join();

  std::cout << "All threads finished. Exiting." << std::endl;
  return 0;
}