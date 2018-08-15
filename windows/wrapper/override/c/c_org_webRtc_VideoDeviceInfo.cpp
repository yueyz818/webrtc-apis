
/* Generated by zsLibEventingTool */


#ifndef C_USE_GENERATED_ORG_WEBRTC_VIDEODEVICEINFO
#include <wrapper/override/c/c_org_webRtc_VideoDeviceInfo.cpp>
#else /* C_USE_GENERATED_ORG_WEBRTC_VIDEODEVICEINFO */

#include "c_helpers.h"
#include <zsLib/types.h>
#include <zsLib/eventing/types.h>
#include <zsLib/SafeInt.h>

#include "c_org_webRtc_VideoDeviceInfo.h"
#include "../org_webRtc_VideoDeviceInfo.h"

using namespace wrapper;

//------------------------------------------------------------------------------
org_webRtc_VideoDeviceInfo_t WEBRTC_WRAPPER_C_CALLING_CONVENTION org_webRtc_VideoDeviceInfo_wrapperCreate_VideoDeviceInfo()
{
  typedef org_webRtc_VideoDeviceInfo_t CType;
  typedef wrapper::org::webRtc::VideoDeviceInfoPtr WrapperTypePtr;
  auto result = wrapper::org::webRtc::VideoDeviceInfo::wrapper_create();
  result->wrapper_init_org_webRtc_VideoDeviceInfo();
  return reinterpret_cast<CType>(new WrapperTypePtr(result));
}

//------------------------------------------------------------------------------
org_webRtc_VideoDeviceInfo_t WEBRTC_WRAPPER_C_CALLING_CONVENTION org_webRtc_VideoDeviceInfo_wrapperClone(org_webRtc_VideoDeviceInfo_t handle)
{
  typedef wrapper::org::webRtc::VideoDeviceInfoPtr WrapperTypePtr;
  typedef WrapperTypePtr * WrapperTypePtrRawPtr;
  if (0 == handle) return 0;
  return reinterpret_cast<org_webRtc_VideoDeviceInfo_t>(new WrapperTypePtr(*reinterpret_cast<WrapperTypePtrRawPtr>(handle)));
}

//------------------------------------------------------------------------------
void WEBRTC_WRAPPER_C_CALLING_CONVENTION org_webRtc_VideoDeviceInfo_wrapperDestroy(org_webRtc_VideoDeviceInfo_t handle)
{
  typedef wrapper::org::webRtc::VideoDeviceInfoPtr WrapperTypePtr;
  typedef WrapperTypePtr * WrapperTypePtrRawPtr;
  if (0 == handle) return;
  delete reinterpret_cast<WrapperTypePtrRawPtr>(handle);
}

//------------------------------------------------------------------------------
instance_id_t WEBRTC_WRAPPER_C_CALLING_CONVENTION org_webRtc_VideoDeviceInfo_wrapperInstanceId(org_webRtc_VideoDeviceInfo_t handle)
{
  typedef wrapper::org::webRtc::VideoDeviceInfoPtr WrapperTypePtr;
  typedef WrapperTypePtr * WrapperTypePtrRawPtr;
  if (0 == handle) return 0;
  return reinterpret_cast<instance_id_t>((*reinterpret_cast<WrapperTypePtrRawPtr>(handle)).get());
}

//------------------------------------------------------------------------------
zs_Any_t WEBRTC_WRAPPER_C_CALLING_CONVENTION org_webRtc_VideoDeviceInfo_get_info(org_webRtc_VideoDeviceInfo_t wrapperThisHandle)
{
  auto wrapperThis = wrapper::org_webRtc_VideoDeviceInfo_wrapperFromHandle(wrapperThisHandle);
  return wrapper::zs_Any_wrapperToHandle(wrapperThis->get_info());
}


namespace wrapper
{
  //----------------------------------------------------------------------------
  org_webRtc_VideoDeviceInfo_t org_webRtc_VideoDeviceInfo_wrapperToHandle(wrapper::org::webRtc::VideoDeviceInfoPtr value)
  {
    typedef org_webRtc_VideoDeviceInfo_t CType;
    typedef wrapper::org::webRtc::VideoDeviceInfoPtr WrapperTypePtr;
    typedef WrapperTypePtr * WrapperTypePtrRawPtr;
    if (!value) return 0;
    return reinterpret_cast<CType>(new WrapperTypePtr(value));
  }

  //----------------------------------------------------------------------------
  wrapper::org::webRtc::VideoDeviceInfoPtr org_webRtc_VideoDeviceInfo_wrapperFromHandle(org_webRtc_VideoDeviceInfo_t handle)
  {
    typedef wrapper::org::webRtc::VideoDeviceInfoPtr WrapperTypePtr;
    typedef WrapperTypePtr * WrapperTypePtrRawPtr;
    if (0 == handle) return WrapperTypePtr();
    return (*reinterpret_cast<WrapperTypePtrRawPtr>(handle));
  }


} /* namespace wrapper */

#endif /* ifndef C_USE_GENERATED_ORG_WEBRTC_VIDEODEVICEINFO */