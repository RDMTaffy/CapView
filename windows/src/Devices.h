// Capture-device discovery and video/audio pairing.
#pragma once

#include "Common.h"

struct VideoDeviceInfo {
    std::wstring name;          // MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME
    std::wstring symbolicLink;  // MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK (unique, port-dependent)
    std::wstring model;         // "VID:PID" from the symbolic link, e.g. "07CA:3112"; may be empty
    std::wstring containerId;   // physical device container GUID string; may be empty
    bool isVirtual = false;     // software camera (OBS Virtual Camera etc.): never auto-selected
    bool looksLikeCaptureCard = false;  // offers >= 1280 wide at >= 50 fps
};

struct AudioInputInfo {
    std::wstring id;           // IMMDevice::GetId (endpoint id string)
    std::wstring name;         // PKEY_Device_FriendlyName, e.g. "디지털 오디오 인터페이스(StreamLine Mini+ GC311G2)"
    std::wstring deviceName;   // PKEY_DeviceInterface_FriendlyName (the adapter name), e.g. "StreamLine Mini+ GC311G2"
    std::wstring model;        // "VID:PID" if derivable; may be empty
    std::wstring containerId;  // PKEY_Device_ContainerId as GUID string; may be empty
};

/// All active video capture devices (Media Foundation). Requires MFStartup to have run.
std::vector<VideoDeviceInfo> EnumerateVideoDevices();

/// All active WASAPI capture endpoints. Requires COM initialised on the calling thread.
std::vector<AudioInputInfo> EnumerateAudioInputs();

/// The audio input that belongs to the same physical capture card as `video`, matched by
/// container id, then USB VID:PID, then name (audio deviceName == video name, or audio name
/// contains video name). Returns an index into `inputs`.
std::optional<size_t> PairAudioInput(const VideoDeviceInfo& video, const std::vector<AudioInputInfo>& inputs);
