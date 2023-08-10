/*
 * Copyright (C) 2022 The Android Open Source Project
 * Copyright 2023 NXP.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "ExtCamPrvdr"
// #define LOG_NDEBUG 0

#include "ExternalCameraProvider.h"

#include <ExternalCameraDevice.h>
#include <aidl/android/hardware/camera/common/Status.h>
#include <convert.h>
#include <cutils/properties.h>
#include <linux/videodev2.h>
#include <log/log.h>
#include <sys/inotify.h>
#include <regex>
#include <android-base/file.h>
#include <android-base/strings.h>
#include <android-base/stringprintf.h>


namespace android {
namespace hardware {
namespace camera {
namespace provider {
namespace implementation {

using base::ReadFileToString;
using ::aidl::android::hardware::camera::common::Status;
using ::android::hardware::camera::device::implementation::ExternalCameraDevice;
using ::android::hardware::camera::device::implementation::fromStatus;
using ::android::hardware::camera::external::common::ExternalCameraConfig;

namespace {
// "device@<version>/external/<id>"
const std::regex kDeviceNameRE("device@([0-9]+\\.[0-9]+)/external/(.+)");
const int kMaxDevicePathLen = 256;
constexpr char kDevicePath[] = "/dev/";
constexpr char kPrefix[] = "video";
constexpr int kPrefixLen = sizeof(kPrefix) - 1;
constexpr int kDevicePrefixLen = sizeof(kDevicePath) + kPrefixLen - 1;

bool matchDeviceName(int cameraIdOffset, const std::string& deviceName, std::string* deviceVersion,
                     std::string* cameraDevicePath) {
    std::smatch sm;
    if (std::regex_match(deviceName, sm, kDeviceNameRE)) {
        if (deviceVersion != nullptr) {
            *deviceVersion = sm[1];
        }
        if (cameraDevicePath != nullptr) {
            *cameraDevicePath = "/dev/video" + std::to_string(std::stoi(sm[2]) - cameraIdOffset);
        }
        return true;
    }
    return false;
}
}  // namespace

ExternalCameraProvider::ExternalCameraProvider() : mCfg(ExternalCameraConfig::loadFromCfg()) {
    mHotPlugThread = std::make_shared<HotplugThread>(this);
    mHotPlugThread->run();
}

ExternalCameraProvider::~ExternalCameraProvider() {
    mHotPlugThread->requestExitAndWait();
}

ndk::ScopedAStatus ExternalCameraProvider::setCallback(
        const std::shared_ptr<ICameraProviderCallback>& in_callback) {
    {
        Mutex::Autolock _l(mLock);
        mCallback = in_callback;
    }

    if (mCallback == nullptr) {
        return fromStatus(Status::OK);
    }

    for (const auto& pair : mCameraStatusMap) {
        mCallback->cameraDeviceStatusChange(pair.first, pair.second);
    }
    return fromStatus(Status::OK);
}

ndk::ScopedAStatus ExternalCameraProvider::getVendorTags(
        std::vector<VendorTagSection>* _aidl_return) {
    if (_aidl_return == nullptr) {
        return fromStatus(Status::ILLEGAL_ARGUMENT);
    }
    // No vendor tag support for USB camera
    *_aidl_return = {};
    return fromStatus(Status::OK);
}

ndk::ScopedAStatus ExternalCameraProvider::getCameraIdList(std::vector<std::string>* _aidl_return) {
    if (_aidl_return == nullptr) {
        return fromStatus(Status::ILLEGAL_ARGUMENT);
    }
    // External camera HAL always report 0 camera, and extra cameras
    // are just reported via cameraDeviceStatusChange callbacks
    *_aidl_return = {};
    return fromStatus(Status::OK);
}

ndk::ScopedAStatus ExternalCameraProvider::getCameraDeviceInterface(
        const std::string& in_cameraDeviceName, std::shared_ptr<ICameraDevice>* _aidl_return) {
    if (_aidl_return == nullptr) {
        return fromStatus(Status::ILLEGAL_ARGUMENT);
    }
    std::string cameraDevicePath, deviceVersion;
    bool match = matchDeviceName(mCfg.cameraIdOffset, in_cameraDeviceName, &deviceVersion,
                                 &cameraDevicePath);

    if (!match) {
        *_aidl_return = nullptr;
        return fromStatus(Status::ILLEGAL_ARGUMENT);
    }

    if (mCameraStatusMap.count(in_cameraDeviceName) == 0 ||
        mCameraStatusMap[in_cameraDeviceName] != CameraDeviceStatus::PRESENT) {
        *_aidl_return = nullptr;
        return fromStatus(Status::ILLEGAL_ARGUMENT);
    }

    ALOGV("Constructing external camera device");
    std::shared_ptr<ExternalCameraDevice> deviceImpl =
            ndk::SharedRefBase::make<ExternalCameraDevice>(cameraDevicePath, mCfg);
    if (deviceImpl == nullptr || deviceImpl->isInitFailed()) {
        ALOGE("%s: camera device %s init failed!", __FUNCTION__, cameraDevicePath.c_str());
        *_aidl_return = nullptr;
        return fromStatus(Status::INTERNAL_ERROR);
    }

    IF_ALOGV() {
        int interfaceVersion;
        deviceImpl->getInterfaceVersion(&interfaceVersion);
        ALOGV("%s: device interface version: %d", __FUNCTION__, interfaceVersion);
    }

    *_aidl_return = deviceImpl;
    return fromStatus(Status::OK);
}

ndk::ScopedAStatus ExternalCameraProvider::notifyDeviceStateChange(int64_t) {
    return fromStatus(Status::OK);
}

ndk::ScopedAStatus ExternalCameraProvider::getConcurrentCameraIds(
        std::vector<ConcurrentCameraIdCombination>* _aidl_return) {
    if (_aidl_return == nullptr) {
        return fromStatus(Status::ILLEGAL_ARGUMENT);
    }
    *_aidl_return = {};
    return fromStatus(Status::OK);
}

ndk::ScopedAStatus ExternalCameraProvider::isConcurrentStreamCombinationSupported(
        const std::vector<CameraIdAndStreamCombination>&, bool* _aidl_return) {
    if (_aidl_return == nullptr) {
        return fromStatus(Status::ILLEGAL_ARGUMENT);
    }
    // No concurrent stream combinations are supported
    *_aidl_return = false;
    return fromStatus(Status::OK);
}

void ExternalCameraProvider::addExternalCamera(const char* devName) {
    ALOGV("%s: ExtCam: adding %s to External Camera HAL!", __FUNCTION__, devName);
    Mutex::Autolock _l(mLock);
    std::string deviceName;
    std::string cameraId =
            std::to_string(mCfg.cameraIdOffset + std::atoi(devName + kDevicePrefixLen));
    deviceName =
            std::string("device@") + ExternalCameraDevice::kDeviceVersion + "/external/" + cameraId;
    mCameraStatusMap[deviceName] = CameraDeviceStatus::PRESENT;
    if (mCallback != nullptr) {
        mCallback->cameraDeviceStatusChange(deviceName, CameraDeviceStatus::PRESENT);
    }
}

void ExternalCameraProvider::deviceAdded(const char* devName) {
    {
        base::unique_fd fd(::open(devName, O_RDWR));
        if (fd.get() < 0) {
            ALOGE("%s open v4l2 device %s failed:%s", __FUNCTION__, devName, strerror(errno));
            return;
        }

        struct v4l2_capability capability;
        int ret = ioctl(fd.get(), VIDIOC_QUERYCAP, &capability);
        if (ret < 0) {
            ALOGE("%s v4l2 QUERYCAP %s failed", __FUNCTION__, devName);
            return;
        }

        if (!(capability.device_caps & (V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_CAPTURE_MPLANE))) {
            ALOGW("%s device %s does not support VIDEO_CAPTURE", __FUNCTION__, devName);
            return;
        }
    }

    // See if we can initialize ExternalCameraDevice correctly
    std::shared_ptr<ExternalCameraDevice> deviceImpl =
            ndk::SharedRefBase::make<ExternalCameraDevice>(devName, mCfg);
    if (deviceImpl == nullptr || deviceImpl->isInitFailed()) {
        ALOGW("%s: Attempt to init camera device %s failed!", __FUNCTION__, devName);
        return;
    }
    deviceImpl.reset();
    addExternalCamera(devName);
}

void ExternalCameraProvider::deviceRemoved(const char* devName) {
    Mutex::Autolock _l(mLock);
    std::string deviceName;
    std::string cameraId =
            std::to_string(mCfg.cameraIdOffset + std::atoi(devName + kDevicePrefixLen));

    deviceName =
            std::string("device@") + ExternalCameraDevice::kDeviceVersion + "/external/" + cameraId;

    if (mCameraStatusMap.erase(deviceName) == 0) {
        // Unknown device, do not fire callback
        ALOGE("%s: cannot find camera device to remove %s", __FUNCTION__, devName);
        return;
    }

    if (mCallback != nullptr) {
        mCallback->cameraDeviceStatusChange(deviceName, CameraDeviceStatus::NOT_PRESENT);
    }
}

void ExternalCameraProvider::updateAttachedCameras() {
    ALOGV("%s start scanning for existing V4L2 devices", __FUNCTION__);

    // Find existing /dev/video* devices
    DIR* devdir = opendir(kDevicePath);
    if (devdir == nullptr) {
        ALOGE("%s: cannot open %s! Exiting threadloop", __FUNCTION__, kDevicePath);
        return;
    }

    struct dirent* de;
    while ((de = readdir(devdir)) != nullptr) {
        // Find external v4l devices that's existing before we start watching and add them
        if (!strncmp(kPrefix, de->d_name, kPrefixLen)) {
            std::string deviceId(de->d_name + kPrefixLen);
            if (mCfg.mInternalDevices.count(deviceId) == 0) {
                ALOGV("Non-internal v4l device %s found", de->d_name);
                char v4l2DevicePath[kMaxDevicePathLen];
                char mCamDevice[kMaxDevicePathLen];
                snprintf(v4l2DevicePath, kMaxDevicePathLen, "%s%s", kDevicePath, de->d_name);
                sprintf(mCamDevice, "/sys/class/video4linux/%s/name", de->d_name);
                if(isExternalDevice(v4l2DevicePath, mCamDevice, NULL))
                    deviceAdded(v4l2DevicePath);
            }
        }
    }
    closedir(devdir);
}

bool ExternalCameraProvider::isExternalDevice(const char* devName, const char* sysClassName, bool *isHdmiRx) {
    int32_t ret = -1;
    struct v4l2_capability vidCap;

    if (isHdmiRx) {
        *isHdmiRx = false;
    }

    std::string video_name;
    std::string video_decoder_name = "amphion-vpu-decoder";
    std::string video_encoder_name = "amphion-vpu-encoder";
    if (!ReadFileToString(std::string(sysClassName), &video_name)) {
        ALOGE("can't read video device name");
        return false;
    }
    if ((video_decoder_name.compare(0, video_decoder_name.length(), video_name, 0, video_decoder_name.length()) == 0) ||
        (video_encoder_name.compare(0, video_encoder_name.length(), video_name, 0, video_encoder_name.length()) == 0)) {
        return false;
    }

    base::unique_fd fd(::open(devName, O_RDWR | O_NONBLOCK));
    if (fd.get() < 0) {
        ALOGE("%s open dev path:%s failed:%s", __func__, devName,strerror(errno));
        return false;
    }

    ret = ioctl(fd.get(), VIDIOC_QUERYCAP, &vidCap);
    if (ret < 0) {
         ALOGE("%s QUERYCAP dev path:%s failed", __func__, devName);
         return false;
    }

    if(strstr((const char*)vidCap.driver, "uvc")) {
        struct v4l2_fmtdesc vid_fmtdesc;
        vid_fmtdesc.index = 0;
        vid_fmtdesc.type  = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        ret = ioctl(fd.get(), VIDIOC_ENUM_FMT, &vid_fmtdesc);
        if(ret == 0) {
            return true;
        }
        ALOGE("Although %s driver name has uvc, but it's a uvc meta device", devName);
    } else if(strstr((const char*)vidCap.driver, "cap")) {
        //HDMI RX for mek_8qm
        std::string buffer;
        std::string propName = "mxc_isi.6.capture"; // TODO: Here is hardcoded for hdmi rx
        if (!ReadFileToString(std::string(sysClassName), &buffer)) {
            ALOGE("can't read video device name");
            return false;
        }

        // string read from ReadFileToString have '\n' in last byte
        if (propName.compare(0,propName.length(),buffer,0,propName.length()) == 0) {
            if (isHdmiRx) {
                *isHdmiRx = true;
            }
            return true;
        }
    }

    return false;
}

// Start ExternalCameraProvider::HotplugThread functions

ExternalCameraProvider::HotplugThread::HotplugThread(ExternalCameraProvider* parent)
    : mParent(parent), mInternalDevices(parent->mCfg.mInternalDevices) {}

ExternalCameraProvider::HotplugThread::~HotplugThread() {
    // Clean up inotify descriptor if needed.
    if (mINotifyFD >= 0) {
        close(mINotifyFD);
    }
}

bool ExternalCameraProvider::HotplugThread::initialize() {
    // Update existing cameras
    mParent->updateAttachedCameras();

    // Set up non-blocking fd. The threadLoop will be responsible for polling read at the
    // desired frequency
    mINotifyFD = inotify_init();
    if (mINotifyFD < 0) {
        ALOGE("%s: inotify init failed! Exiting threadloop", __FUNCTION__);
        return false;
    }

    // Start watching /dev/ directory for created and deleted files
    mWd = inotify_add_watch(mINotifyFD, kDevicePath, IN_CREATE | IN_DELETE);
    if (mWd < 0) {
        ALOGE("%s: inotify add watch failed! Exiting threadloop", __FUNCTION__);
        return false;
    }

    mPollFd = {.fd = mINotifyFD, .events = POLLIN};

    mIsInitialized = true;
    return true;
}

bool ExternalCameraProvider::HotplugThread::threadLoop() {
    // Initialize inotify descriptors if needed.
    if (!mIsInitialized && !initialize()) {
        return true;
    }

    // poll /dev/* and handle timeouts and error
    int pollRet = poll(&mPollFd, /* fd_count= */ 1, /* timeout= */ 250);
    if (pollRet == 0) {
        // no read event in 100ms
        mPollFd.revents = 0;
        return true;
    } else if (pollRet < 0) {
        ALOGE("%s: error while polling for /dev/*: %d", __FUNCTION__, errno);
        mPollFd.revents = 0;
        return true;
    } else if (mPollFd.revents & POLLERR) {
        ALOGE("%s: polling /dev/ returned POLLERR", __FUNCTION__);
        mPollFd.revents = 0;
        return true;
    } else if (mPollFd.revents & POLLHUP) {
        ALOGE("%s: polling /dev/ returned POLLHUP", __FUNCTION__);
        mPollFd.revents = 0;
        return true;
    } else if (mPollFd.revents & POLLNVAL) {
        ALOGE("%s: polling /dev/ returned POLLNVAL", __FUNCTION__);
        mPollFd.revents = 0;
        return true;
    }
    // mPollFd.revents must contain POLLIN, so safe to reset it before reading
    mPollFd.revents = 0;

    uint64_t offset = 0;
    ssize_t ret = read(mINotifyFD, mEventBuf, sizeof(mEventBuf));
    if (ret < sizeof(struct inotify_event)) {
        // invalid event. skip
        return true;
    }

    char mHdmiRxNode[kMaxDevicePathLen];
    while (offset < ret) {
        struct inotify_event* event = (struct inotify_event*)&mEventBuf[offset];
        offset += sizeof(struct inotify_event) + event->len;

        if (event->wd != mWd) {
            // event for an unrelated descriptor. ignore.
            continue;
        }

        ALOGI("%s inotify_event %s", __FUNCTION__, event->name);

        if (!strncmp("cec", event->name, 3)) {
            // if the event is cec, need to find hdmi-rx node from /dev/video* devices
            bool isHdmiRx = false;
            char v4l2DevicePath[kMaxDevicePathLen];
            char mCamDevice[kMaxDevicePathLen];
            if (event->mask & IN_CREATE) {
                DIR* devdir = opendir(kDevicePath);
                struct dirent* de;
                if(devdir == 0) {
                    ALOGE("%s: cannot open %s! Exiting threadloop", __FUNCTION__, kDevicePath);
                    return false;
                }
                while ((de = readdir(devdir)) != 0) {
                    if (!strncmp(kPrefix, de->d_name, kPrefixLen)) {
                        snprintf(v4l2DevicePath, kMaxDevicePathLen,
                                "%s%s", kDevicePath, de->d_name);
                        sprintf(mCamDevice, "/sys/class/video4linux/%s/name", de->d_name);
                        // hdmi-rx is not ready until 800ms.
                        usleep(800000);
                        if(mParent->isExternalDevice(v4l2DevicePath, mCamDevice, &isHdmiRx) && isHdmiRx) {
                            strncpy(mHdmiRxNode, v4l2DevicePath, strlen(v4l2DevicePath));
                            mParent->deviceAdded(v4l2DevicePath);
                            ALOGI("%s: add mHdmiRxNode------:%s", __FUNCTION__, mHdmiRxNode);
                            break;
                        }
                    }
                }
                closedir(devdir);
            } else if (event->mask & IN_DELETE) {
                mParent->deviceRemoved(mHdmiRxNode);
                ALOGI("%s: rmv mHdmiRxNode-------:%s", __FUNCTION__, mHdmiRxNode);
            }
        }

        if (strncmp(kPrefix, event->name, kPrefixLen) != 0) {
            // event not for /dev/video*. ignore.
            continue;
        }

        std::string deviceId = event->name + kPrefixLen;
        if (mInternalDevices.count(deviceId) != 0) {
            // update to an internal device. ignore.
            continue;
        }

        char v4l2DevicePath[kMaxDevicePathLen];
        char mCamDevice[kMaxDevicePathLen];
        snprintf(v4l2DevicePath, kMaxDevicePathLen, "%s%s", kDevicePath, event->name);
        sprintf(mCamDevice, "/sys/class/video4linux/%s/name", event->name);
        if (event->mask & IN_CREATE) {
            // usb camera is not ready until 100ms.
            usleep(100000);
            if(mParent->isExternalDevice(v4l2DevicePath, mCamDevice, NULL))
                mParent->deviceAdded(v4l2DevicePath);
        } else if (event->mask & IN_DELETE) {
                mParent->deviceRemoved(v4l2DevicePath);
        }
    }
    return true;
}

// End ExternalCameraProvider::HotplugThread functions

}  // namespace implementation
}  // namespace provider
}  // namespace camera
}  // namespace hardware
}  // namespace android
