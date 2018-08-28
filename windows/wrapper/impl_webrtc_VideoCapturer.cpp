
#include "impl_webrtc_VideoCapturer.h"

#ifdef WINUWP
#ifdef CPPWINRT_VERSION

#include <ppltasks.h>

#include <string>
#include <vector>
#include <Mferror.h>

#include <zsLib/String.h>
#include <zsLib/IMessageQueueThread.h>

#include <wrapper/impl_org_webRtc_pre_include.h>
#include "media/base/videocommon.h"
#include "rtc_base/logging.h"
#include <wrapper/impl_org_webRtc_post_include.h>
#include "rtc_base/Win32.h"
#include "libyuv/planar_functions.h"
#include "common_video/libyuv/include/webrtc_libyuv.h"
#include "common_video/video_common_winuwp.h"
#include "api/video/i420_buffer.h"

#include <winrt/windows.media.capture.h>
#include <winrt/windows.devices.enumeration.h>

using zsLib::String;
using zsLib::Time;
using zsLib::Seconds;
using zsLib::Milliseconds;
using zsLib::AutoRecursiveLock;

using winrt::Windows::Devices::Enumeration::DeviceClass;
using winrt::Windows::Devices::Enumeration::DeviceInformation;
using winrt::Windows::Devices::Enumeration::DeviceInformationCollection;
using winrt::Windows::Devices::Enumeration::Panel;
using winrt::Windows::Graphics::Display::DisplayOrientations;
using winrt::Windows::Graphics::Display::DisplayInformation;
using winrt::Windows::Media::Capture::MediaCapture;
using winrt::Windows::Media::Capture::MediaCaptureFailedEventArgs;
using winrt::Windows::Media::Capture::MediaCaptureFailedEventHandler;
using winrt::Windows::Media::Capture::MediaCaptureInitializationSettings;
using winrt::Windows::Media::Capture::MediaStreamType;
using winrt::Windows::Media::IMediaExtension;
using winrt::Windows::Media::MediaProperties::IVideoEncodingProperties;
using winrt::Windows::Media::MediaProperties::MediaEncodingProfile;
using winrt::Windows::Media::MediaProperties::MediaEncodingSubtypes;
using winrt::Windows::Media::MediaProperties::VideoEncodingProperties;
using winrt::Windows::UI::Core::DispatchedHandler;
using winrt::Windows::UI::Core::CoreDispatcherPriority;
using winrt::Windows::Foundation::IAsyncAction;
using winrt::Windows::Foundation::TypedEventHandler;
using winrt::Windows::System::Threading::ThreadPoolTimer;
using winrt::Windows::System::Threading::TimerElapsedHandler;
using winrt::Windows::Foundation::Collections::IVectorView;

namespace webrtc
{

  //-----------------------------------------------------------------------------
  void RunOnCoreDispatcher(std::function<void()> fn, bool async) {
    winrt::Windows::UI::Core::CoreDispatcher windowDispatcher =
      VideoCommonWinUWP::GetCoreDispatcher();
    if (windowDispatcher != nullptr) {
      auto handler = winrt::Windows::UI::Core::DispatchedHandler([fn]() {
        fn();
      });
      auto action = windowDispatcher.RunAsync(
        CoreDispatcherPriority::Normal, handler);
      if (async) {
        Concurrency::create_task([action]() { action.get(); });
      } else {
        Concurrency::create_task([action]() { action.get(); }).wait();
      }
    } else {
      fn();
    }
  }

  //-----------------------------------------------------------------------------
  AppStateDispatcher* AppStateDispatcher::instance_ = NULL;

  //-----------------------------------------------------------------------------
  AppStateDispatcher* AppStateDispatcher::Instance() {
    if (!instance_)
      instance_ = new AppStateDispatcher;
    return instance_;
  }

  //-----------------------------------------------------------------------------
  AppStateDispatcher::AppStateDispatcher() :
    display_orientation_(DisplayOrientations::Portrait) {
  }

  //-----------------------------------------------------------------------------
  void AppStateDispatcher::DisplayOrientationChanged(
    DisplayOrientations display_orientation) {
    display_orientation_ = display_orientation;
    for (auto obs_it = observers_.begin(); obs_it != observers_.end(); ++obs_it) {
      (*obs_it)->DisplayOrientationChanged(display_orientation);
    }
  }

  //-----------------------------------------------------------------------------
  DisplayOrientations AppStateDispatcher::GetOrientation() const {
    return display_orientation_;
  }

  //-----------------------------------------------------------------------------
  void AppStateDispatcher::AddObserver(AppStateObserver* observer) {
    observers_.push_back(observer);
  }

  //-----------------------------------------------------------------------------
  void AppStateDispatcher::RemoveObserver(AppStateObserver* observer) {
    for (auto obs_it = observers_.begin(); obs_it != observers_.end(); ++obs_it) {
      if (*obs_it == observer) {
        observers_.erase(obs_it);
        break;
      }
    }
  }

  //-----------------------------------------------------------------------------
  class DisplayOrientation {
  public:
    virtual ~DisplayOrientation();

  public:
    DisplayOrientation(DisplayOrientationListener* listener);
    void OnOrientationChanged(
      winrt::Windows::Graphics::Display::DisplayInformation const& sender,
      winrt::Windows::Foundation::IInspectable const& args);

    winrt::Windows::Graphics::Display::DisplayOrientations Orientation() { return orientation_; }
  private:
    DisplayOrientationListener * listener_;
    winrt::Windows::Graphics::Display::DisplayInformation display_info_{ nullptr };
    winrt::Windows::Graphics::Display::DisplayOrientations orientation_
    { winrt::Windows::Graphics::Display::DisplayOrientations::None };
    winrt::event_token
      orientation_changed_registration_token_;
  };

  //-----------------------------------------------------------------------------
  DisplayOrientation::~DisplayOrientation() {
    auto tmpDisplayInfo = display_info_;
    auto tmpToken = orientation_changed_registration_token_;
    if (tmpDisplayInfo != nullptr) {
      RunOnCoreDispatcher([tmpDisplayInfo, tmpToken]() {
        tmpDisplayInfo.OrientationChanged(tmpToken);
      }, true);  // Run async because it can deadlock with core thread.
    }
  }

  //-----------------------------------------------------------------------------
  DisplayOrientation::DisplayOrientation(DisplayOrientationListener* listener)
    : listener_(listener) {
    RunOnCoreDispatcher([this]() {
      // GetForCurrentView() only works on a thread associated with
      // a CoreWindow.  If this doesn't work because we're running in
      // a background task then the orientation needs to come from the
      // foreground as a notification.
      try {
        display_info_ = DisplayInformation::GetForCurrentView();
        orientation_ = display_info_.CurrentOrientation();
        orientation_changed_registration_token_ =
          display_info_.OrientationChanged(
            TypedEventHandler<DisplayInformation,
            winrt::Windows::Foundation::IInspectable>(this, &DisplayOrientation::OnOrientationChanged));
      } catch (...) {
        display_info_ = nullptr;
        orientation_ = winrt::Windows::Graphics::Display::DisplayOrientations::Portrait;
        RTC_LOG(LS_ERROR) << "DisplayOrientation could not be initialized.";
      }
    });
  }

  //-----------------------------------------------------------------------------
  void DisplayOrientation::OnOrientationChanged(DisplayInformation const& sender,
    winrt::Windows::Foundation::IInspectable const& /*args*/) {
    orientation_ = sender.CurrentOrientation();
    if (listener_)
      listener_->OnDisplayOrientationChanged(sender.CurrentOrientation());
  }

  //-----------------------------------------------------------------------------
  class CaptureDevice : public VideoCaptureMediaSinkProxyListener {
  public:
    virtual ~CaptureDevice();

  public:
    CaptureDevice(CaptureDeviceListener* capture_device_listener);

    void Initialize(winrt::hstring const& device_id);

    void CleanupSink();

    void CleanupMediaCapture();

    void Cleanup();

    void StartCapture(winrt::Windows::Media::MediaProperties::MediaEncodingProfile const& media_encoding_profile,
      winrt::Windows::Media::MediaProperties::IVideoEncodingProperties const& video_encoding_properties);

    void StopCapture();

    bool CaptureStarted() { return capture_started_; }

    VideoCaptureCapability GetFrameInfo() { return frame_info_; }

    void OnCaptureFailed(winrt::Windows::Media::Capture::MediaCapture const&  sender,
      winrt::Windows::Media::Capture::MediaCaptureFailedEventArgs const& error_event_args);

    void OnMediaSampleEvent(std::shared_ptr<MediaSampleEventArgs> args) override;

    winrt::agile_ref<winrt::Windows::Media::Capture::MediaCapture> MediaCapture();

    winrt::agile_ref<winrt::Windows::Media::Capture::MediaCapture>
      GetMediaCapture(winrt::hstring const& device_id);

    void RemoveMediaCapture(winrt::hstring const& device_id);

  private:
    void RemovePaddingPixels(uint8_t* video_frame, size_t& video_frame_length);

  private:
    winrt::agile_ref<winrt::Windows::Media::Capture::MediaCapture> media_capture_;
    winrt::hstring device_id_;
    std::shared_ptr<VideoCaptureMediaSinkProxy> media_sink_;
    winrt::event_token
      media_capture_failed_event_registration_token_;
    winrt::event_token
      media_sink_video_sample_event_registration_token_;

    CaptureDeviceListener* capture_device_listener_;

    std::map<winrt::hstring,
      winrt::agile_ref<winrt::Windows::Media::Capture::MediaCapture> >
      media_capture_map_;

    bool capture_started_;
    VideoCaptureCapability frame_info_;
    std::unique_ptr<webrtc::EventWrapper> _stopped;
  };

  //-----------------------------------------------------------------------------
  CaptureDevice::CaptureDevice(
    CaptureDeviceListener* capture_device_listener)
    : media_capture_(nullptr),
    capture_device_listener_(capture_device_listener),
    capture_started_(false) {
    _stopped.reset(webrtc::EventWrapper::Create());
    _stopped->Set();
  }

  //-----------------------------------------------------------------------------
  CaptureDevice::~CaptureDevice() {
  }

  //-----------------------------------------------------------------------------
  void CaptureDevice::Initialize(winrt::hstring const& device_id) {
    RTC_LOG(LS_INFO) << "CaptureDevice::Initialize";
    device_id_ = device_id;
  }

  //-----------------------------------------------------------------------------
  void CaptureDevice::CleanupSink() {
    if (media_sink_ != nullptr) {
      media_sink_.reset();
      capture_started_ = false;
    }
  }

  //-----------------------------------------------------------------------------
  void CaptureDevice::CleanupMediaCapture() {
    winrt::Windows::Media::Capture::MediaCapture media_capture = media_capture_.get();
    if (media_capture != nullptr) {
      media_capture.Failed(media_capture_failed_event_registration_token_);
      RemoveMediaCapture(device_id_);
      media_capture_ = nullptr;
    }
  }

  //-----------------------------------------------------------------------------
  winrt::agile_ref<winrt::Windows::Media::Capture::MediaCapture> CaptureDevice::MediaCapture() {
    return media_capture_;
  }

  //-----------------------------------------------------------------------------
  void CaptureDevice::Cleanup() {
    winrt::Windows::Media::Capture::MediaCapture media_capture = media_capture_.get();
    if (media_capture == nullptr) {
      return;
    }
    if (capture_started_) {
      if (_stopped->Wait(5000) == kEventTimeout) {
        Concurrency::create_task([this, media_capture]() {
          media_capture.StopRecordAsync().get();
        }).then([this](Concurrency::task<void> async_info) {
          try {
            async_info.get();
            CleanupSink();
            CleanupMediaCapture();
            device_id_.clear();
            _stopped->Set();
          } catch (winrt::hresult_error const& /*e*/) {
            CleanupSink();
            CleanupMediaCapture();
            device_id_.clear();
            _stopped->Set();
            throw;
          }
        }).wait();
      }
    } else {
      CleanupSink();
      CleanupMediaCapture();
      device_id_.clear();
    }
  }

  //-----------------------------------------------------------------------------
  void CaptureDevice::StartCapture(
    MediaEncodingProfile const& media_encoding_profile,
    IVideoEncodingProperties const& video_encoding_properties) {
    if (capture_started_) {
      winrt::throw_hresult(ERROR_INVALID_STATE);
    }

    if (_stopped->Wait(5000) == kEventTimeout) {
      winrt::throw_hresult(ERROR_INVALID_STATE);
    }

    CleanupSink();
    CleanupMediaCapture();

    if (device_id_.empty()) {
      RTC_LOG(LS_WARNING) << "Capture device is not initialized.";
      return;
    }

    frame_info_.width = media_encoding_profile.Video().Width();
    frame_info_.height = media_encoding_profile.Video().Height();
    frame_info_.maxFPS =
      static_cast<int>(
        static_cast<float>(
          media_encoding_profile.Video().FrameRate().Numerator()) /
        static_cast<float>(
          media_encoding_profile.Video().FrameRate().Denominator()));
    if (_wcsicmp(media_encoding_profile.Video().Subtype().c_str(),
      MediaEncodingSubtypes::Yv12().c_str()) == 0) {
      frame_info_.videoType = VideoType::kYV12;
    } else if (_wcsicmp(media_encoding_profile.Video().Subtype().c_str(),
      MediaEncodingSubtypes::Yuy2().c_str()) == 0) {
      frame_info_.videoType = VideoType::kYUY2;
    } else if (_wcsicmp(media_encoding_profile.Video().Subtype().c_str(),
      MediaEncodingSubtypes::Iyuv().c_str()) == 0) {
      frame_info_.videoType = VideoType::kIYUV;
    } else if (_wcsicmp(media_encoding_profile.Video().Subtype().c_str(),
      MediaEncodingSubtypes::Rgb24().c_str()) == 0) {
      frame_info_.videoType = VideoType::kRGB24;
    } else if (_wcsicmp(media_encoding_profile.Video().Subtype().c_str(),
      MediaEncodingSubtypes::Rgb32().c_str()) == 0) {
      frame_info_.videoType = VideoType::kARGB;
    } else if (_wcsicmp(media_encoding_profile.Video().Subtype().c_str(),
      MediaEncodingSubtypes::Mjpg().c_str()) == 0) {
      frame_info_.videoType = VideoType::kMJPEG;
    } else if (_wcsicmp(media_encoding_profile.Video().Subtype().c_str(),
      MediaEncodingSubtypes::Nv12().c_str()) == 0) {
      frame_info_.videoType = VideoType::kNV12;
    } else {
      frame_info_.videoType = VideoType::kUnknown;
    }

    media_capture_ = GetMediaCapture(device_id_);
    media_capture_failed_event_registration_token_ =
      media_capture_.get().Failed(
        MediaCaptureFailedEventHandler(this,
        &CaptureDevice::OnCaptureFailed));

#ifdef WIN10
    // Tell the video device controller to optimize for Low Latency then Power consumption
    media_capture_.get().VideoDeviceController().DesiredOptimization(
      winrt::Windows::Media::Devices::MediaCaptureOptimization::LatencyThenPower);
#endif

    media_sink_ = std::make_shared<VideoCaptureMediaSinkProxy>(std::shared_ptr<CaptureDevice>(this));

    auto initOp = media_sink_->InitializeAsync(media_encoding_profile.Video());
    auto initTask = Concurrency::create_task([this, initOp]() {
      return initOp.get();
      }).then([this, media_encoding_profile,
        video_encoding_properties](IMediaExtension const& media_extension) {
      auto setPropOp =
        media_capture_.get().VideoDeviceController().SetMediaStreamPropertiesAsync(
          MediaStreamType::VideoRecord, video_encoding_properties);
      return Concurrency::create_task([this, setPropOp]() {
        return setPropOp.get();
        }).then([this, media_encoding_profile, media_extension]() {
        auto startRecordOp = media_capture_.get().StartRecordToCustomSinkAsync(
          media_encoding_profile, media_extension);
        return Concurrency::create_task([this, startRecordOp]() {
          return startRecordOp.get();
          });
      });
    });

    initTask.then([this](Concurrency::task<void> async_info) {
      try {
        async_info.get();
        capture_started_ = true;
      } catch (winrt::hresult_error const& e) {
        RTC_LOG(LS_ERROR) << "StartRecordToCustomSinkAsync exception: "
          << rtc::ToUtf8(e.message().c_str());
        CleanupSink();
        CleanupMediaCapture();
      }
    }).wait();

    RTC_LOG(LS_INFO) << "CaptureDevice::StartCapture: returning";
  }

  //-----------------------------------------------------------------------------
  void CaptureDevice::StopCapture() {
    if (!capture_started_) {
      RTC_LOG(LS_INFO) << "CaptureDevice::StopCapture: called when never started";
      return;
    }

    Concurrency::create_task([this]() {
      return media_capture_.get().StopRecordAsync().get();
      }).then([this](Concurrency::task<void> async_info) {
      try {
        async_info.get();
        CleanupSink();
        CleanupMediaCapture();
        _stopped->Set();
      } catch (winrt::hresult_error const& e) {
        CleanupSink();
        CleanupMediaCapture();
        _stopped->Set();
        RTC_LOG(LS_ERROR) <<
          "CaptureDevice::StopCapture: Stop failed, reason: '" <<
          rtc::ToUtf8(e.message().c_str()) << "'";
      }
    });
  }

  //-----------------------------------------------------------------------------
  void CaptureDevice::OnCaptureFailed(
    winrt::Windows::Media::Capture::MediaCapture const& /*sender*/,
    MediaCaptureFailedEventArgs const& error_event_args) {
    if (capture_device_listener_) {
      capture_device_listener_->OnCaptureDeviceFailed(
        error_event_args.Code(),
        error_event_args.Message());
    }
  }

  //-----------------------------------------------------------------------------
  void CaptureDevice::OnMediaSampleEvent(std::shared_ptr<MediaSampleEventArgs> args)
  {
    if (capture_device_listener_) {
      winrt::com_ptr<IMFSample> spMediaSample = args->GetMediaSample();
      winrt::com_ptr<IMFMediaBuffer> spMediaBuffer;
      HRESULT hr = spMediaSample->GetBufferByIndex(0, spMediaBuffer.put());
      LONGLONG hnsSampleTime = 0;
      BYTE* pbBuffer = NULL;
      DWORD cbMaxLength = 0;
      DWORD cbCurrentLength = 0;

      if (SUCCEEDED(hr)) {
        hr = spMediaSample->GetSampleTime(&hnsSampleTime);
      }
      if (SUCCEEDED(hr)) {
        hr = spMediaBuffer->Lock(&pbBuffer, &cbMaxLength, &cbCurrentLength);
      }
      if (SUCCEEDED(hr)) {
        uint8_t* video_frame;
        size_t video_frame_length;
        int64_t capture_time;
        video_frame = pbBuffer;
        video_frame_length = cbCurrentLength;
        // conversion from 100-nanosecond to millisecond units
        capture_time = hnsSampleTime / 10000;

        RemovePaddingPixels(video_frame, video_frame_length);

        RTC_LOG(LS_VERBOSE) <<
          "Video Capture - Media sample received - video frame length: " <<
          video_frame_length << ", capture time : " << capture_time;

        capture_device_listener_->OnIncomingFrame(video_frame,
          video_frame_length,
          frame_info_);

        hr = spMediaBuffer->Unlock();
      }
      if (FAILED(hr)) {
        RTC_LOG(LS_ERROR) << "Failed to send media sample. " << hr;
      }
    }
  }

  //-----------------------------------------------------------------------------
  winrt::agile_ref<MediaCapture>
    CaptureDevice::GetMediaCapture(winrt::hstring const& device_id) {

    // We cache MediaCapture objects
    auto iter = media_capture_map_.find(device_id);
    if (iter != media_capture_map_.end()) {
      return iter->second;
    } else {
#if (defined(WINAPI_FAMILY) && (WINAPI_FAMILY == WINAPI_FAMILY_PHONE_APP || \
                                defined(WINDOWS_PHONE_APP)))
      // WINDOWS_PHONE_APP is defined at gyp level to overcome the missing
      // WINAPI_FAMILY when building with VS2015

      // On some Windows Phone 8 devices, two calls of InitializeAsync on two
      // different coexisting instances causes exception to be thrown from the
      // second call.
      // Since after calling the second InitializeAsync all further calls fail
      // with exception, we maintain a maximum of one MediaCapture instance
      // in cache.
      // The behavior is present on Lumia620, OS version 8.10.14219.341 and
      // 10.0.10586.36
      media_capture_map_.clear();
#endif
      winrt::agile_ref<winrt::Windows::Media::Capture::MediaCapture>
        media_capture_agile = winrt::Windows::Media::Capture::MediaCapture();

      Concurrency::task<void> initialize_async_task;
      auto handler = DispatchedHandler(
        [this, &initialize_async_task, media_capture_agile, device_id]() {
        auto settings = MediaCaptureInitializationSettings();
        settings.VideoDeviceId(device_id);

        // If Communications media category is configured, the
        // GetAvailableMediaStreamProperties will report only H264 frame format
        // for some devices (ex: Surface Pro 3). Since at the moment, WebRTC does
        // not support receiving H264 frames from capturer, the Communications
        // category is not configured.

        // settings.MediaCategory(
        //  winrt::Windows::Media::Capture::MediaCategory::Communications);
        auto initOp = media_capture_agile.get().InitializeAsync(settings);
        initialize_async_task = Concurrency::create_task([this, initOp]() {
            return initOp.get();
          }).then([this, media_capture_agile](Concurrency::task<void> initTask) {
          try {
            initTask.get();
          } catch (winrt::hresult_error const& e) {
            RTC_LOG(LS_ERROR)
              << "Failed to initialize media capture device. "
              << rtc::ToUtf8(e.message().c_str());
          }
        });
      });

      winrt::Windows::UI::Core::CoreDispatcher windowDispatcher = VideoCommonWinUWP::GetCoreDispatcher();
      if (windowDispatcher != nullptr) {
        auto dispatcher_action = windowDispatcher.RunAsync(
          CoreDispatcherPriority::Normal, handler);
        Concurrency::create_task([this, dispatcher_action]() { return dispatcher_action.get(); }).wait();
      } else {
        handler();
      }

      initialize_async_task.wait();

      // Cache the MediaCapture object so we don't recreate it later.
      media_capture_map_[device_id] = media_capture_agile;
      return media_capture_agile;
    }
  }

  //-----------------------------------------------------------------------------
  void CaptureDevice::RemoveMediaCapture(winrt::hstring const& device_id) {

    auto iter = media_capture_map_.find(device_id);
    if (iter != media_capture_map_.end()) {
      media_capture_map_.erase(iter);
    }
  }

  //-----------------------------------------------------------------------------
  void CaptureDevice::RemovePaddingPixels(uint8_t* video_frame,
    size_t& video_frame_length) {

    int padded_row_num = 16 - frame_info_.height % 16;
    int padded_col_num = 16 - frame_info_.width % 16;
    if (padded_row_num == 16)
      padded_row_num = 0;
    if (padded_col_num == 16)
      padded_col_num = 0;

    if (frame_info_.videoType == VideoType::kYV12 &&
      (int32_t)video_frame_length >
      frame_info_.width * frame_info_.height * 3 / 2) {
      uint8_t* src_video_y = video_frame;
      uint8_t* src_video_v = src_video_y +
        (frame_info_.width + padded_col_num) *
        (frame_info_.height + padded_row_num);
      uint8_t* src_video_u = src_video_v +
        (frame_info_.width + padded_col_num) *
        (frame_info_.height + padded_row_num) / 4;
      uint8_t* dst_video_y = src_video_y;
      uint8_t* dst_video_v = dst_video_y +
        frame_info_.width * frame_info_.height;
      uint8_t* dst_video_u = dst_video_v +
        frame_info_.width * frame_info_.height / 4;
      video_frame_length = frame_info_.width * frame_info_.height * 3 / 2;
      libyuv::CopyPlane(src_video_y, frame_info_.width + padded_col_num,
        dst_video_y, frame_info_.width,
        frame_info_.width, frame_info_.height);
      libyuv::CopyPlane(src_video_v, (frame_info_.width + padded_col_num) / 2,
        dst_video_v, frame_info_.width / 2,
        frame_info_.width / 2, frame_info_.height / 2);
      libyuv::CopyPlane(src_video_u, (frame_info_.width + padded_col_num) / 2,
        dst_video_u, frame_info_.width / 2,
        frame_info_.width / 2, frame_info_.height / 2);
    } else if (frame_info_.videoType == VideoType::kYUY2 &&
      (int32_t)video_frame_length >
      frame_info_.width * frame_info_.height * 2) {
      uint8_t* src_video = video_frame;
      uint8_t* dst_video = src_video;
      video_frame_length = frame_info_.width * frame_info_.height * 2;
      libyuv::CopyPlane(src_video, 2 * (frame_info_.width + padded_col_num),
        dst_video, 2 * frame_info_.width,
        2 * frame_info_.width, frame_info_.height);
    } else if (frame_info_.videoType == VideoType::kIYUV &&
      (int32_t)video_frame_length >
      frame_info_.width * frame_info_.height * 3 / 2) {
      uint8_t* src_video_y = video_frame;
      uint8_t* src_video_u = src_video_y +
        (frame_info_.width + padded_col_num) *
        (frame_info_.height + padded_row_num);
      uint8_t* src_video_v = src_video_u +
        (frame_info_.width + padded_col_num) *
        (frame_info_.height + padded_row_num) / 4;
      uint8_t* dst_video_y = src_video_y;
      uint8_t* dst_video_u = dst_video_y +
        frame_info_.width * frame_info_.height;
      uint8_t* dst_video_v = dst_video_u +
        frame_info_.width * frame_info_.height / 4;
      video_frame_length = frame_info_.width * frame_info_.height * 3 / 2;
      libyuv::CopyPlane(src_video_y, frame_info_.width + padded_col_num,
        dst_video_y, frame_info_.width,
        frame_info_.width, frame_info_.height);
      libyuv::CopyPlane(src_video_u, (frame_info_.width + padded_col_num) / 2,
        dst_video_u, frame_info_.width / 2,
        frame_info_.width / 2, frame_info_.height / 2);
      libyuv::CopyPlane(src_video_v, (frame_info_.width + padded_col_num) / 2,
        dst_video_v, frame_info_.width / 2,
        frame_info_.width / 2, frame_info_.height / 2);
    } else if (frame_info_.videoType == VideoType::kRGB24 &&
      (int32_t)video_frame_length >
      frame_info_.width * frame_info_.height * 3) {
      uint8_t* src_video = video_frame;
      uint8_t* dst_video = src_video;
      video_frame_length = frame_info_.width * frame_info_.height * 3;
      libyuv::CopyPlane(src_video, 3 * (frame_info_.width + padded_col_num),
        dst_video, 3 * frame_info_.width,
        3 * frame_info_.width, frame_info_.height);
    } else if (frame_info_.videoType == VideoType::kARGB &&
      (int32_t)video_frame_length >
      frame_info_.width * frame_info_.height * 4) {
      uint8_t* src_video = video_frame;
      uint8_t* dst_video = src_video;
      video_frame_length = frame_info_.width * frame_info_.height * 4;
      libyuv::CopyPlane(src_video, 4 * (frame_info_.width + padded_col_num),
        dst_video, 4 * frame_info_.width,
        4 * frame_info_.width, frame_info_.height);
    } else if (frame_info_.videoType == VideoType::kNV12 &&
      (int32_t)video_frame_length >
      frame_info_.width * frame_info_.height * 3 / 2) {
      uint8_t* src_video_y = video_frame;
      uint8_t* src_video_uv = src_video_y +
        (frame_info_.width + padded_col_num) *
        (frame_info_.height + padded_row_num);
      uint8_t* dst_video_y = src_video_y;
      uint8_t* dst_video_uv = dst_video_y +
        frame_info_.width * frame_info_.height;
      video_frame_length = frame_info_.width * frame_info_.height * 3 / 2;
      libyuv::CopyPlane(src_video_y, frame_info_.width + padded_col_num,
        dst_video_y, frame_info_.width,
        frame_info_.width, frame_info_.height);
      libyuv::CopyPlane(src_video_uv, frame_info_.width + padded_col_num,
        dst_video_uv, frame_info_.width,
        frame_info_.width, frame_info_.height / 2);
    }
  }

  //-----------------------------------------------------------------------------
  class BlackFramesGenerator {
  public:
    virtual ~BlackFramesGenerator();

    BlackFramesGenerator(CaptureDeviceListener* capture_device_listener);

    void StartCapture(const VideoCaptureCapability& frame_info);

    void StopCapture();

    bool CaptureStarted() { return capture_started_; }

    void Cleanup();

    VideoCaptureCapability GetFrameInfo() { return frame_info_; }

  private:
    CaptureDeviceListener * capture_device_listener_;
    bool capture_started_;
    VideoCaptureCapability frame_info_;
    ThreadPoolTimer timer_ { nullptr };
  };

  //-----------------------------------------------------------------------------
  BlackFramesGenerator::BlackFramesGenerator(
    CaptureDeviceListener* capture_device_listener) :
    capture_started_(false), capture_device_listener_(capture_device_listener) {
  }

  //-----------------------------------------------------------------------------
  BlackFramesGenerator::~BlackFramesGenerator() {
    Cleanup();
  }

  //-----------------------------------------------------------------------------
  void BlackFramesGenerator::StartCapture(
    const VideoCaptureCapability& frame_info) {
    frame_info_ = frame_info;
    frame_info_.videoType = VideoType::kRGB24;

    if (capture_started_) {
      RTC_LOG(LS_INFO) << "Black frame generator already started";
      winrt::throw_hresult(ERROR_INVALID_STATE);
    }
    RTC_LOG(LS_INFO) << "Starting black frame generator";

    size_t black_frame_size = frame_info_.width * frame_info_.height * 3;
    std::shared_ptr<std::vector<uint8_t>> black_frame(
      new std::vector<uint8_t>(black_frame_size, 0));
    auto handler = TimerElapsedHandler(
      [this, black_frame_size, black_frame]
    (ThreadPoolTimer const& /*timer*/) -> void {
      if (this->capture_device_listener_ != nullptr) {
        this->capture_device_listener_->OnIncomingFrame(
          black_frame->data(),
          black_frame_size,
          this->frame_info_);
      }
    });
    int64_t timespan_value = (int64_t)((1000 * 1000 * 10 /*1s in hns*/) /
      frame_info_.maxFPS);
    auto timespan = winrt::Windows::Foundation::TimeSpan(timespan_value);
    timer_ = ThreadPoolTimer::CreatePeriodicTimer(handler, timespan);
    capture_started_ = true;
  }

  //-----------------------------------------------------------------------------
  void BlackFramesGenerator::StopCapture() {
    if (!capture_started_) {
      winrt::throw_hresult(ERROR_INVALID_STATE);
    }

    RTC_LOG(LS_INFO) << "Stopping black frame generator";
    timer_.Cancel();
    timer_ = nullptr;
    capture_started_ = false;
  }

  //-----------------------------------------------------------------------------
  void BlackFramesGenerator::Cleanup() {
    capture_device_listener_ = nullptr;
    if (capture_started_) {
      StopCapture();
    }
  }

  //-----------------------------------------------------------------------------
  VideoCapturer::VideoCapturer(const make_private &) :
    device_(nullptr),
    camera_location_(Panel::Unknown),
    display_orientation_(nullptr),
    fake_device_(nullptr),
    last_frame_info_(),
    video_encoding_properties_(nullptr),
    media_encoding_profile_(nullptr),
    subscriptions_(decltype(subscriptions_)::create())
  {
    if (VideoCommonWinUWP::GetCoreDispatcher() == nullptr) {
      RTC_LOG(LS_INFO) << "Using AppStateDispatcher as orientation source";
      AppStateDispatcher::Instance()->AddObserver(this);
    } else {
      // DisplayOrientation needs access to UI thread.
      RTC_LOG(LS_INFO) << "Using local detection for orientation source";
      display_orientation_ = std::make_shared<DisplayOrientation>(this);
    }
  }

  //-----------------------------------------------------------------------------
  VideoCapturer::~VideoCapturer()
  {
    thisWeak_.reset();

    if (deviceUniqueId_ != nullptr)
      delete[] deviceUniqueId_;
    if (device_ != nullptr)
      device_->Cleanup();
    if (fake_device_ != nullptr) {
      fake_device_->Cleanup();
    }
    if (display_orientation_ == nullptr) {
      AppStateDispatcher::Instance()->RemoveObserver(this);
    }
  }

  //-----------------------------------------------------------------------------
  VideoCapturerPtr VideoCapturer::create(const CreationProperties &info) noexcept
  {
    auto result = std::make_shared<VideoCapturer>(make_private{});
    result->thisWeak_ = result;
    result->init(info);
    return result;
  }

  //-----------------------------------------------------------------------------
  void VideoCapturer::init(const CreationProperties &props) noexcept
  {
    id_ = String(props.id_);
    externalCapture_ = props.externalCapture_;

    if (props.delegate_) {
      defaultSubscription_ = subscriptions_.subscribe(props.delegate_, zsLib::IMessageQueueThread::singletonUsingCurrentGUIThreadsMessageQueue());
    }

    winrt::Windows::Media::MediaProperties::VideoEncodingProperties properties{ nullptr };

    auto thisWeak = thisWeak_;

    rtc::CritScope cs(&apiCs_);
    const char* device_unique_id_utf8 = props.id_;
    const int32_t device_unique_id_length = (int32_t)strlen(props.id_);
    if (device_unique_id_length > kVideoCaptureUniqueNameLength) {
      RTC_LOG(LS_ERROR) << "Device name too long";
      return;
    }

    RTC_LOG(LS_INFO) << "Init called for device " << props.id_;

    device_id_.clear();

    deviceUniqueId_ = new (std::nothrow) char[device_unique_id_length + 1];
    memcpy(deviceUniqueId_, props.id_, device_unique_id_length + 1);

    Concurrency::create_task([this]() {
      return DeviceInformation::FindAllAsync(DeviceClass::VideoCapture).get().as<IVectorView<DeviceInformation> >();
      }).then([this, device_unique_id_utf8, device_unique_id_length](
        IVectorView<DeviceInformation> const& collection) {
        try {
        DeviceInformationCollection dev_info_collection = collection.as<DeviceInformationCollection>();
        if (dev_info_collection == nullptr || dev_info_collection.Size() == 0) {
          RTC_LOG_F(LS_ERROR) << "No video capture device found";
          return;
        }
        // Look for the device in the collection.
        DeviceInformation chosen_dev_info = nullptr;
        for (unsigned int i = 0; i < dev_info_collection.Size(); i++) {
          auto dev_info = dev_info_collection.GetAt(i);
          if (rtc::ToUtf8(dev_info.Id().c_str()) == device_unique_id_utf8) {
            device_id_ = dev_info.Id();
            if (dev_info.EnclosureLocation() != nullptr) {
              camera_location_ = dev_info.EnclosureLocation().Panel();
            } else {
              camera_location_ = Panel::Unknown;
            }
            break;
          }
        }
      } catch (winrt::hresult_error const& e) {
        RTC_LOG(LS_ERROR)
          << "Failed to retrieve device info collection. "
          << rtc::ToUtf8(e.message().c_str());
      }
    }).wait();

    if (device_id_.empty()) {
      RTC_LOG(LS_ERROR) << "No video capture device found";
      return;
    }

    device_ = std::make_shared<CaptureDevice>(this);

    device_->Initialize(device_id_);

    fake_device_ = std::make_shared<BlackFramesGenerator>(this);
  }

  //-----------------------------------------------------------------------------
  IVideoCapturerSubscriptionPtr VideoCapturer::subscribe(IVideoCapturerDelegatePtr originalDelegate)
  {
    AutoRecursiveLock lock(lock_);
    if (!originalDelegate) return defaultSubscription_;

    auto subscription = subscriptions_.subscribe(originalDelegate, zsLib::IMessageQueueThread::singletonUsingCurrentGUIThreadsMessageQueue());

    auto delegate = subscriptions_.delegate(subscription, true);

    if (delegate) {
      auto pThis = thisWeak_.lock();
    }

    return subscription;
  }

  //-----------------------------------------------------------------------------
  int32_t VideoCapturer::startCapture(
    const VideoCaptureCapability& capability) {
    rtc::CritScope cs(&apiCs_);
    winrt::hstring subtype;
    switch (capability.videoType) {
    case VideoType::kYV12:
      subtype = MediaEncodingSubtypes::Yv12();
      break;
    case VideoType::kYUY2:
      subtype = MediaEncodingSubtypes::Yuy2();
      break;
    case VideoType::kI420:
    case VideoType::kIYUV:
      subtype = MediaEncodingSubtypes::Iyuv();
      break;
    case VideoType::kRGB24:
      subtype = MediaEncodingSubtypes::Rgb24();
      break;
    case VideoType::kARGB:
      subtype = MediaEncodingSubtypes::Argb32();
      break;
    case VideoType::kMJPEG:
      // MJPEG format is decoded internally by MF engine to NV12
      subtype = MediaEncodingSubtypes::Nv12();
      break;
    case VideoType::kNV12:
      subtype = MediaEncodingSubtypes::Nv12();
      break;
    default:
      RTC_LOG(LS_ERROR) <<
        "The specified raw video format is not supported on this plaform.";
      return -1;
    }

    media_encoding_profile_ = MediaEncodingProfile();
    media_encoding_profile_.Audio(nullptr);
    media_encoding_profile_.Container(nullptr);
    media_encoding_profile_.Video(VideoEncodingProperties::CreateUncompressed(
      subtype, capability.width, capability.height));
    media_encoding_profile_.Video().FrameRate().Numerator(capability.maxFPS);
    media_encoding_profile_.Video().FrameRate().Denominator(1);

    video_encoding_properties_ = nullptr;
    int min_width_diff = INT_MAX;
    int min_height_diff = INT_MAX;
    int min_fps_diff = INT_MAX;
    auto mediaCapture = device_->GetMediaCapture(device_id_);
    auto streamProperties =
      mediaCapture.get().VideoDeviceController().GetAvailableMediaStreamProperties(
        MediaStreamType::VideoRecord);
    for (unsigned int i = 0; i < streamProperties.Size(); i++) {
      IVideoEncodingProperties prop;
      streamProperties.GetAt(i).as(prop);

      if (capability.videoType != VideoType::kMJPEG &&
        _wcsicmp(prop.Subtype().c_str(), subtype.c_str()) != 0 ||
        capability.videoType == VideoType::kMJPEG &&
        _wcsicmp(prop.Subtype().c_str(),
          MediaEncodingSubtypes::Mjpg().c_str()) != 0) {
        continue;
      }

      int width_diff = abs(static_cast<int>(prop.Width() - capability.width));
      int height_diff = abs(static_cast<int>(prop.Height() - capability.height));
      int prop_fps = static_cast<int>(
        (static_cast<float>(prop.FrameRate().Numerator()) /
          static_cast<float>(prop.FrameRate().Denominator())));
      int fps_diff = abs(static_cast<int>(prop_fps - capability.maxFPS));

      if (width_diff < min_width_diff) {
        video_encoding_properties_ = prop;
        min_width_diff = width_diff;
        min_height_diff = height_diff;
        min_fps_diff = fps_diff;
      } else if (width_diff == min_width_diff) {
        if (height_diff < min_height_diff) {
          video_encoding_properties_ = prop;
          min_height_diff = height_diff;
          min_fps_diff = fps_diff;
        } else if (height_diff == min_height_diff) {
          if (fps_diff < min_fps_diff) {
            video_encoding_properties_ = prop;
            min_fps_diff = fps_diff;
          }
        }
      }
    }
    try {
      if (display_orientation_) {
        ApplyDisplayOrientation(display_orientation_->Orientation());
      } else {
        ApplyDisplayOrientation(AppStateDispatcher::Instance()->GetOrientation());
      }
      device_->StartCapture(media_encoding_profile_,
        video_encoding_properties_);
      last_frame_info_ = capability;
    } catch (winrt::hresult_error const& e) {
      RTC_LOG(LS_ERROR) << "Failed to start capture. "
        << rtc::ToUtf8(e.message().c_str());
      return -1;
    }

    return 0;
  }

  //-----------------------------------------------------------------------------
  int32_t VideoCapturer::stopCapture() {
    rtc::CritScope cs(&apiCs_);

    try {
      if (device_->CaptureStarted()) {
        device_->StopCapture();
      }
      if (fake_device_->CaptureStarted()) {
        fake_device_->StopCapture();
      }
    } catch (winrt::hresult_error const& e) {
      RTC_LOG(LS_ERROR) << "Failed to stop capture. "
        << rtc::ToUtf8(e.message().c_str());
      return -1;
    }
    return 0;
  }

  //-----------------------------------------------------------------------------
  bool VideoCapturer::captureStarted() {
    rtc::CritScope cs(&apiCs_);

    return device_->CaptureStarted() || fake_device_->CaptureStarted();
  }

  //-----------------------------------------------------------------------------
  int32_t VideoCapturer::captureSettings(VideoCaptureCapability& settings) {
    rtc::CritScope cs(&apiCs_);
    settings = device_->GetFrameInfo();
    return 0;
  }

  //-----------------------------------------------------------------------------
  bool VideoCapturer::suspendCapture() {
    if (device_->CaptureStarted()) {
      RTC_LOG(LS_INFO) << "SuspendCapture";
      device_->StopCapture();
      fake_device_->StartCapture(last_frame_info_);
      return true;
    }
    RTC_LOG(LS_INFO) << "SuspendCapture capture is not started";
    return false;
  }

  //-----------------------------------------------------------------------------
  bool VideoCapturer::resumeCapture() {
    if (fake_device_->CaptureStarted()) {
      RTC_LOG(LS_INFO) << "ResumeCapture";
      fake_device_->StopCapture();
      device_->StartCapture(media_encoding_profile_,
        video_encoding_properties_);
      return true;
    }
    RTC_LOG(LS_INFO) << "ResumeCapture, capture is not started";
    return false;
  }

  //-----------------------------------------------------------------------------
  bool VideoCapturer::isSuspended() {
    return fake_device_->CaptureStarted();
  }

  //-----------------------------------------------------------------------------
  void VideoCapturer::DisplayOrientationChanged(
    winrt::Windows::Graphics::Display::DisplayOrientations display_orientation) {
    if (display_orientation_ != nullptr) {
      RTC_LOG(LS_WARNING) <<
        "Ignoring orientation change notification from AppStateDispatcher";
      return;
    }
    ApplyDisplayOrientation(display_orientation);
  }

  //-----------------------------------------------------------------------------
  void VideoCapturer::OnDisplayOrientationChanged(
    DisplayOrientations orientation) {
    ApplyDisplayOrientation(orientation);
  }

  //-----------------------------------------------------------------------------
  void VideoCapturer::OnIncomingFrame(
    uint8_t* videoFrame,
    size_t videoFrameLength,
    const VideoCaptureCapability& frameInfo) {
    if (device_->CaptureStarted()) {
      last_frame_info_ = frameInfo;
    }
    rtc::CritScope cs(&apiCs_);

    const int32_t width = frameInfo.width;
    const int32_t height = frameInfo.height;

    // Not encoded, convert to I420.
    if (frameInfo.videoType != VideoType::kMJPEG &&
      CalcBufferSize(frameInfo.videoType, width, abs(height)) !=
      videoFrameLength) {
      RTC_LOG(LS_ERROR) << "Wrong incoming frame length.";
      return;
    }

    int stride_y = width;
    int stride_uv = (width + 1) / 2;
    int target_width = width;
    int target_height = height;

    // SetApplyRotation doesn't take any lock. Make a local copy here.
    bool apply_rotation = apply_rotation_;

    if (apply_rotation) {
      // Rotating resolution when for 90/270 degree rotations.
      if (rotateFrame_ == kVideoRotation_90 ||
        rotateFrame_ == kVideoRotation_270) {
        target_width = abs(height);
        target_height = width;
      }
    }

    rtc::scoped_refptr<I420Buffer> buffer = I420Buffer::Create(
      target_width, abs(target_height), stride_y, stride_uv, stride_uv);

    libyuv::RotationMode rotation_mode = libyuv::kRotate0;
    if (apply_rotation) {
      switch (rotateFrame_) {
      case kVideoRotation_0:
        rotation_mode = libyuv::kRotate0;
        break;
      case kVideoRotation_90:
        rotation_mode = libyuv::kRotate90;
        break;
      case kVideoRotation_180:
        rotation_mode = libyuv::kRotate180;
        break;
      case kVideoRotation_270:
        rotation_mode = libyuv::kRotate270;
        break;
      }
    }

    const int conversionResult = libyuv::ConvertToI420(
      videoFrame, videoFrameLength, buffer.get()->MutableDataY(),
      buffer.get()->StrideY(), buffer.get()->MutableDataU(),
      buffer.get()->StrideU(), buffer.get()->MutableDataV(),
      buffer.get()->StrideV(), 0, 0,  // No Cropping
      width, height, target_width, target_height, rotation_mode,
      ConvertVideoType(frameInfo.videoType));
    if (conversionResult < 0) {
      RTC_LOG(LS_ERROR) << "Failed to convert capture frame from type "
        << static_cast<int>(frameInfo.videoType) << "to I420.";
      return;
    }

    int64_t captureTime = 0;
    VideoFrame captureFrame(buffer, 0, rtc::TimeMillis(),
      !apply_rotation ? rotateFrame_ : kVideoRotation_0);
    captureFrame.set_ntp_time_ms(captureTime);

    externalCapture_->IncomingFrame(videoFrame, videoFrameLength, frameInfo, captureTime);
  }

  //-----------------------------------------------------------------------------
  void VideoCapturer::OnCaptureDeviceFailed(HRESULT code,
    winrt::hstring const& message) {
    RTC_LOG(LS_ERROR) << "Capture device failed. HRESULT: " <<
      code << " Message: " << rtc::ToUtf8(message.c_str());
    rtc::CritScope cs(&apiCs_);
    if (device_ != nullptr && device_->CaptureStarted()) {
      try {
        device_->StopCapture();
      } catch (winrt::hresult_error const& ex) {
        RTC_LOG(LS_WARNING) <<
          "Capture device failed: failed to stop ex='"
          << rtc::ToUtf8(ex.message().c_str()) << "'";
      }
    }
  }

  //-----------------------------------------------------------------------------
  void VideoCapturer::ApplyDisplayOrientation(
    DisplayOrientations orientation) {
    if (camera_location_ == winrt::Windows::Devices::Enumeration::Panel::Unknown)
      return;
    rtc::CritScope cs(&apiCs_);
    switch (orientation) {
    case winrt::Windows::Graphics::Display::DisplayOrientations::Portrait:
      if (camera_location_ == winrt::Windows::Devices::Enumeration::Panel::Front)
        rotateFrame_ = VideoRotation::kVideoRotation_270;
      else
        rotateFrame_ = VideoRotation::kVideoRotation_90;
      break;
    case winrt::Windows::Graphics::Display::DisplayOrientations::PortraitFlipped:
      if (camera_location_ == winrt::Windows::Devices::Enumeration::Panel::Front)
        rotateFrame_ = VideoRotation::kVideoRotation_90;
      else
        rotateFrame_ = VideoRotation::kVideoRotation_270;
      break;
    case winrt::Windows::Graphics::Display::DisplayOrientations::Landscape:
      rotateFrame_ = VideoRotation::kVideoRotation_0;
      break;
    case winrt::Windows::Graphics::Display::DisplayOrientations::LandscapeFlipped:
      rotateFrame_ = VideoRotation::kVideoRotation_180;
      break;
    default:
      rotateFrame_ = VideoRotation::kVideoRotation_0;
      break;
    }
  }

  //-----------------------------------------------------------------------------
  IVideoCapturerPtr IVideoCapturer::create(const CreationProperties &info) noexcept
  {
    return VideoCapturer::create(info);
  }
} // namespace webrtc

#endif //CPPWINRT_VERSION
#endif //WINUWP