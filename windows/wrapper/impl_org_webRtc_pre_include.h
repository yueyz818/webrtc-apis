
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4100)
#pragma warning(disable: 4458)
#pragma warning(disable: 4244)
#pragma warning(disable: 4242)
#pragma warning(disable: 4996)
#pragma warning(disable: 4266)
#endif //_MSC_VER

namespace cricket
{
  enum CaptureState : int;
  enum MediaType : int;

  struct VideoFormat;
}

// forward for webrtc can go here
namespace rtc
{
  class Thread;
  class RTCCertificate;
  class IntervalRange;
  class KeyParams;
  struct SSLFingerprint;

  struct RSAParams;

  enum ECCurve : int;
  enum KeyType : int;
}

namespace webrtc
{
  enum Band : int;
  enum class SdpSemantics;
  enum class SdpType;
  enum class RtpTransceiverDirection;

  struct DataBuffer;
  struct DataChannelInit;
  struct RtpTransceiverInit;
  struct RtpParameters;
  struct RtpEncodingParameters;
  struct RtpRtxParameters;
  struct RtpFecParameters;
  struct RtcpFeedback;
  struct RtpCodecParameters;
  struct RtpCodecCapability;
  struct RtpHeaderExtensionCapability;
  struct RtpExtension;
  struct RtpCapabilities;
  struct RtpParameters;

  class AudioBuffer;
  class AudioFrame;
  class RtpSource;
  class AudioTrackInterface;
  class VideoTrackInterface;
  class DtmfSenderInterface;
  class RtpSenderInterface;
  class RtpReceiverInterface;
  class RtpTransceiverInterface;
  class SessionDescriptionInterface;
  class PeerConnectionInterface;
  class PeerConnectionFactoryInterface;
  class PeerConnectionFactory;

  class MediaSourceInterface;
  class AudioSourceInterface;
  class VideoTrackSourceInterface;

  class RTCError;

  class VideoFrame;

  class VideoFrameBuffer;
  class I420BufferInterface;
  class I420ABufferInterface;
  class I444BufferInterface;
  class I010BufferInterface;
  class NativeHandleBuffer;
  class PlanarYuvBuffer;
  class PlanarYuv8Buffer;
  class PlanarYuv16BBuffer;

  class RTCStats;
  class RTCStatsReport;
  class RTCStatsCollectorCallback;
}
