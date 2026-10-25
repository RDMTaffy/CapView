// Capture-device discovery and video/audio pairing.
//
// A capture card shows up twice: as a UVC camera (Media Foundation VIDCAP source) and as a USB
// audio input (WASAPI capture endpoint). The PnP tree ties them together: both functions share
// the card's container id and its USB VID:PID, which is what PairAudioInput compares.

#include "Devices.h"

#include <objbase.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mmdeviceapi.h>
#include <devicetopology.h>
#include <propsys.h>
#include <cfgmgr32.h>

#include <algorithm>
#include <cwchar>
#include <format>
#include <iterator>
#include <map>

namespace {

// Property keys spelled out here instead of using DEVPKEY_* / PKEY_*: those symbols are only
// defined in a translation unit that includes <initguid.h> first (which would also define every
// other GUID of the headers below), and mingw and MSVC differ in which import library has them.
// Values from devpkey.h / functiondiscoverykeys_devpkey.h.
const DEVPROPKEY kDevKeyInstanceId = {{0x78c34fc8, 0x104a, 0x4aca, {0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57}}, 256};
const DEVPROPKEY kDevKeyContainerId = {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
const PROPERTYKEY kKeyFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const PROPERTYKEY kKeyInterfaceFriendlyName = {{0x026e516e, 0xb814, 0x414b, {0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22}}, 2};
const PROPERTYKEY kKeyContainerId = {{0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};
const PROPERTYKEY kKeyInstanceId = {{0x78c34fc8, 0x104a, 0x4aca, {0x9e, 0xa4, 0x52, 0x4d, 0x52, 0x99, 0x6e, 0x57}}, 256};

// GUID_CONTAINERID_INTERNALLY_CONNECTED_DEVICE: every device built into the PC (integrated
// webcam, internal microphone array, a PCIe capture card) shares it, so it says nothing about
// which audio belongs to which video and is treated as "no container id".
const GUID kContainerInternal = {0x00000000, 0x0000, 0x0000, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};

constexpr UINT32 kCaptureCardMinWidth = 1280;
constexpr double kCaptureCardMinFps = 50.0 - 0.01;  // tolerate rationals that land just below 50

// MARK: Strings

std::wstring Upper(std::wstring text) {
    if (!text.empty()) CharUpperBuffW(text.data(), static_cast<DWORD>(text.size()));
    return text;
}

bool ContainsNoCase(const std::wstring& text, const std::wstring& part) {
    return !part.empty() && Upper(text).find(Upper(part)) != std::wstring::npos;
}

bool EqualsNoCase(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

bool StartsWithNoCase(const std::wstring& text, const wchar_t* prefix) {
    const size_t length = std::wcslen(prefix);
    return text.size() >= length &&
           CompareStringOrdinal(text.c_str(), static_cast<int>(length), prefix, static_cast<int>(length), TRUE) ==
               CSTR_EQUAL;
}

const wchar_t* OrDash(const std::wstring& text) { return text.empty() ? L"-" : text.c_str(); }

/// "{XXXXXXXX-...}" (StringFromGUID2 form, same for MF/cfgmgr32 and WASAPI), or empty for the
/// null GUID and the shared "built into the PC" container.
std::wstring ContainerString(const GUID& guid) {
    if (IsEqualGUID(guid, GUID{}) || IsEqualGUID(guid, kContainerInternal)) return {};
    wchar_t text[64] = {};
    return StringFromGUID2(guid, text, static_cast<int>(std::size(text))) > 0 ? std::wstring(text) : std::wstring();
}

// MARK: PnP tree (cfgmgr32)

/// Instance id of the device that exposes `interfacePath` (an MF symbolic link or a KS filter
/// path), e.g. "USB\VID_07CA&PID_3112&MI_00\7&1A2B3C4D&0&0000". Empty if unknown.
std::wstring InterfaceInstanceId(const std::wstring& interfacePath) {
    if (interfacePath.empty()) return {};
    wchar_t buffer[MAX_DEVICE_ID_LEN + 1] = {};
    ULONG size = sizeof(buffer);
    DEVPROPTYPE type = DEVPROP_TYPE_EMPTY;
    const CONFIGRET cr = CM_Get_Device_Interface_PropertyW(interfacePath.c_str(), &kDevKeyInstanceId, &type,
                                                           reinterpret_cast<PBYTE>(buffer), &size, 0);
    if (cr != CR_SUCCESS || type != DEVPROP_TYPE_STRING) {
        Log(L"Devices: no instance id for interface %ls (CONFIGRET %lu, type 0x%lX)", interfacePath.c_str(),
            static_cast<unsigned long>(cr), static_cast<unsigned long>(type));
        return {};
    }
    buffer[MAX_DEVICE_ID_LEN] = L'\0';
    return buffer;
}

std::optional<DEVINST> LocateDevNode(std::wstring instanceId) {
    if (instanceId.empty()) return std::nullopt;
    DEVINST node = 0;
    const CONFIGRET cr = CM_Locate_DevNodeW(&node, instanceId.data(), CM_LOCATE_DEVNODE_NORMAL);
    if (cr != CR_SUCCESS) {
        Log(L"Devices: CM_Locate_DevNode(%ls) failed (CONFIGRET %lu)", instanceId.c_str(), static_cast<unsigned long>(cr));
        return std::nullopt;
    }
    return node;
}

std::wstring DevNodeInstanceId(DEVINST node) {
    wchar_t buffer[MAX_DEVICE_ID_LEN + 1] = {};
    if (CM_Get_Device_IDW(node, buffer, static_cast<ULONG>(std::size(buffer)), 0) != CR_SUCCESS) return {};
    buffer[MAX_DEVICE_ID_LEN] = L'\0';
    return buffer;
}

std::wstring DevNodeContainerId(DEVINST node) {
    GUID guid{};
    ULONG size = sizeof(guid);
    DEVPROPTYPE type = DEVPROP_TYPE_EMPTY;
    const CONFIGRET cr =
        CM_Get_DevNode_PropertyW(node, &kDevKeyContainerId, &type, reinterpret_cast<PBYTE>(&guid), &size, 0);
    if (cr != CR_SUCCESS || type != DEVPROP_TYPE_GUID || size != sizeof(guid)) return {};
    return ContainerString(guid);
}

struct DevNodeFacts {
    std::wstring instanceId;
    std::wstring model;        // USB "VID:PID" of the device or the USB function it sits on
    std::wstring containerId;  // the device's own (meaningful) container id
};

/// Reads the container id of `node` and finds its USB VID:PID: on the node itself, or on the
/// nearest ancestor reached only through software nodes (SWD\..., e.g. an audio endpoint above
/// its USB audio function). The walk stops at any other bus so a Bluetooth headset doesn't
/// inherit the VID:PID of the USB Bluetooth radio, nor a device that of its USB hub.
DevNodeFacts InspectDevNode(DEVINST node) {
    DevNodeFacts facts;
    facts.instanceId = DevNodeInstanceId(node);
    facts.containerId = DevNodeContainerId(node);
    std::wstring id = facts.instanceId;
    for (int depth = 0; depth < 8; ++depth) {
        facts.model = UsbIdsFromPath(id);
        if (!facts.model.empty()) break;
        if (depth > 0 && !StartsWithNoCase(id, L"SWD\\")) break;
        DEVINST parent = 0;
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) break;
        node = parent;
        id = DevNodeInstanceId(node);
        if (id.empty()) break;
    }
    return facts;
}

std::optional<DevNodeFacts> FactsForInstance(const std::wstring& instanceId) {
    if (auto node = LocateDevNode(instanceId)) return InspectDevNode(*node);
    return std::nullopt;
}

std::optional<DevNodeFacts> FactsForInterface(const std::wstring& interfacePath) {
    return FactsForInstance(InterfaceInstanceId(interfacePath));
}

// MARK: Video

std::wstring AllocatedString(IMFAttributes* attributes, REFGUID key) {
    LPWSTR value = nullptr;
    UINT32 length = 0;
    if (FAILED(attributes->GetAllocatedString(key, &value, &length)) || !value) return {};
    std::wstring text(value, length);
    CoTaskMemFree(value);
    return text;
}

double FrameRate(IMFMediaType* type, REFGUID key) {
    UINT32 numerator = 0, denominator = 0;
    if (FAILED(MFGetAttributeRatio(type, key, &numerator, &denominator)) || denominator == 0) return 0;
    return static_cast<double>(numerator) / denominator;
}

/// Opens the device and scans its native formats for >= 1280 wide at >= 50 fps.
/// nullopt when the formats could not be read (busy, camera access denied, unplugged meanwhile):
/// such a result is not cached so the next enumeration tries again.
std::optional<bool> ProbeCaptureCard(IMFActivate* activate, const std::wstring& name) {
    ComPtr<IMFMediaSource> source;
    HRESULT hr = activate->ActivateObject(IID_PPV_ARGS(&source));
    if (FAILED(hr) || !source) {
        Log(L"Devices: cannot open \"%ls\" to read its formats (hr=0x%08lX)%ls", name.c_str(),
            static_cast<unsigned long>(hr), hr == E_ACCESSDENIED ? L" - camera access denied by privacy settings?" : L"");
        return std::nullopt;
    }
    // Release the device right away on every path (camera LED / privacy indicator, other apps,
    // our own VideoCapture opening it next) instead of whenever the last reference goes.
    struct ShutdownGuard {
        IMFActivate* activate;
        IMFMediaSource* source;
        ~ShutdownGuard() {
            source->Shutdown();
            activate->ShutdownObject();
        }
    } shutdown{activate, source.Get()};

    ComPtr<IMFPresentationDescriptor> descriptor;
    hr = source->CreatePresentationDescriptor(&descriptor);
    if (FAILED(hr) || !descriptor) {
        Log(L"Devices: CreatePresentationDescriptor for \"%ls\" failed (hr=0x%08lX)", name.c_str(),
            static_cast<unsigned long>(hr));
        return std::nullopt;
    }

    DWORD streamCount = 0;
    if (FAILED(LogIfFailed(descriptor->GetStreamDescriptorCount(&streamCount), L"Devices: GetStreamDescriptorCount"))) {
        return std::nullopt;
    }

    UINT32 typeCount = 0;
    UINT32 largestWidth = 0, largestHeight = 0;
    double fastest = 0;
    UINT32 bestWidth = 0, bestHeight = 0;  // widest mode at >= 50 fps
    double bestFps = 0;
    for (DWORD s = 0; s < streamCount; ++s) {
        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> stream;
        if (FAILED(descriptor->GetStreamDescriptorByIndex(s, &selected, &stream)) || !stream) continue;
        ComPtr<IMFMediaTypeHandler> handler;
        if (FAILED(stream->GetMediaTypeHandler(&handler)) || !handler) continue;
        DWORD count = 0;
        if (FAILED(handler->GetMediaTypeCount(&count))) continue;
        for (DWORD t = 0; t < count; ++t) {
            ComPtr<IMFMediaType> type;
            if (FAILED(handler->GetMediaTypeByIndex(t, &type)) || !type) continue;
            GUID major{};
            if (FAILED(type->GetMajorType(&major)) || !IsEqualGUID(major, MFMediaType_Video)) continue;
            UINT32 width = 0, height = 0;
            if (FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height))) continue;
            // Some drivers report a range; the maximum is what the device can reach.
            const double fps =
                std::max(FrameRate(type.Get(), MF_MT_FRAME_RATE), FrameRate(type.Get(), MF_MT_FRAME_RATE_RANGE_MAX));
            ++typeCount;
            if (uint64_t(width) * height > uint64_t(largestWidth) * largestHeight) {
                largestWidth = width;
                largestHeight = height;
            }
            fastest = std::max(fastest, fps);
            if (width >= kCaptureCardMinWidth && fps >= kCaptureCardMinFps &&
                (width > bestWidth || (width == bestWidth && fps > bestFps))) {
                bestWidth = width;
                bestHeight = height;
                bestFps = fps;
            }
        }
    }

    if (typeCount == 0) {
        Log(L"Devices: \"%ls\" reported no video formats (%lu streams)", name.c_str(), static_cast<unsigned long>(streamCount));
        return std::nullopt;
    }
    const bool captureCard = bestWidth > 0;
    if (captureCard) {
        Log(L"Devices: \"%ls\": %u video formats, largest %ux%u, fastest %.2f fps, capture-card mode %ux%u @ %.2f",
            name.c_str(), typeCount, largestWidth, largestHeight, fastest, bestWidth, bestHeight, bestFps);
    } else {
        Log(L"Devices: \"%ls\": %u video formats, largest %ux%u, fastest %.2f fps, no %u-wide mode at 50 fps",
            name.c_str(), typeCount, largestWidth, largestHeight, fastest, kCaptureCardMinWidth);
    }
    return captureCard;
}

/// Probe results by upper-cased symbolic link. Opening a camera is slow (and lights its privacy
/// indicator), and the device list is rebuilt on every device change and menu, so each device
/// is opened once per run; a different device on the same port has a different link.
struct ProbeCache {
    std::mutex mutex;
    std::map<std::wstring, bool> results;
};

ProbeCache& Probes() {
    static ProbeCache cache;
    return cache;
}

bool LooksLikeCaptureCard(IMFActivate* activate, const VideoDeviceInfo& device) {
    const std::wstring key = Upper(device.symbolicLink);
    ProbeCache& cache = Probes();
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        if (auto it = cache.results.find(key); it != cache.results.end()) return it->second;
    }
    // Not under the lock: probing can take a second and concurrent probes are harmless.
    const std::optional<bool> result = ProbeCaptureCard(activate, device.name);
    if (!result) return false;
    std::lock_guard<std::mutex> lock(cache.mutex);
    cache.results[key] = *result;
    return *result;
}

// MARK: Audio

std::wstring StringProperty(IPropertyStore* store, const PROPERTYKEY& key) {
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring text;
    if (SUCCEEDED(store->GetValue(key, &value)) && value.vt == VT_LPWSTR && value.pwszVal) text = value.pwszVal;
    PropVariantClear(&value);
    return text;
}

std::wstring ContainerProperty(IPropertyStore* store, const PROPERTYKEY& key) {
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring text;
    if (SUCCEEDED(store->GetValue(key, &value))) {
        if (value.vt == VT_CLSID && value.puuid) {
            text = ContainerString(*value.puuid);
        } else if (value.vt == VT_LPWSTR && value.pwszVal) {  // not expected, but cheap to accept
            GUID guid{};
            if (SUCCEEDED(CLSIDFromString(value.pwszVal, &guid))) text = ContainerString(guid);
        }
    }
    PropVariantClear(&value);
    return text;
}

/// Device id of the audio adapter (KS filter) the endpoint is connected to, from the documented
/// device topology API, e.g. "{2}.\\?\usb#vid_07ca&pid_3112&mi_01#...#{6994ad04-...}\global".
/// Empty for endpoints without a hardware connection (virtual audio drivers).
std::wstring ConnectedAdapterId(IMMDevice* device) {
    ComPtr<IDeviceTopology> topology;
    if (FAILED(device->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(topology.GetAddressOf()))) ||
        !topology) {
        return {};
    }
    UINT connectors = 0;
    if (FAILED(topology->GetConnectorCount(&connectors)) || connectors == 0) return {};
    ComPtr<IConnector> connector;
    if (FAILED(topology->GetConnector(0, &connector)) || !connector) return {};
    LPWSTR id = nullptr;
    if (FAILED(connector->GetDeviceIdConnectedTo(&id)) || !id) return {};
    std::wstring result(id);
    CoTaskMemFree(id);
    return result;
}

/// Turns a topology device id into a device interface path cfgmgr32 accepts: drops the
/// "{2}." provider prefix and a "/00010001"-style subdevice suffix after the reference string.
std::wstring InterfacePathFromTopologyId(std::wstring id) {
    if (!id.empty() && id[0] == L'{') {
        if (const size_t end = id.find(L"}."); end != std::wstring::npos && end < 16) id.erase(0, end + 2);
    }
    const size_t lastBackslash = id.rfind(L'\\');
    if (const size_t slash = id.find(L'/', lastBackslash == std::wstring::npos ? 0 : lastBackslash);
        slash != std::wstring::npos) {
        id.erase(slash);
    }
    return id;
}

}  // namespace

// MARK: Public

std::vector<VideoDeviceInfo> EnumerateVideoDevices() {
    std::vector<VideoDeviceInfo> devices;
    const ULONGLONG started = GetTickCount64();

    ComPtr<IMFAttributes> attributes;
    if (FAILED(LogIfFailed(MFCreateAttributes(&attributes, 1), L"Devices: MFCreateAttributes"))) return devices;
    if (FAILED(LogIfFailed(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                               MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID),
                           L"Devices: set VIDCAP source type"))) {
        return devices;
    }

    IMFActivate** rawActivates = nullptr;
    UINT32 count = 0;
    const HRESULT hr =
        LogIfFailed(MFEnumDeviceSources(attributes.Get(), &rawActivates, &count), L"Devices: MFEnumDeviceSources(video)");
    // Take ownership of every element (and free the array) before anything can return early.
    std::vector<ComPtr<IMFActivate>> activates;
    if (rawActivates) {
        activates.resize(count);
        for (UINT32 i = 0; i < count; ++i) activates[i].Attach(rawActivates[i]);
        CoTaskMemFree(rawActivates);
    }
    if (FAILED(hr)) return devices;

    for (size_t i = 0; i < activates.size(); ++i) {
        IMFActivate* activate = activates[i].Get();
        if (!activate) continue;

        VideoDeviceInfo info;
        info.symbolicLink = AllocatedString(activate, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
        info.name = AllocatedString(activate, MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
        if (info.symbolicLink.empty()) {
            Log(L"Devices: skipping video source %u \"%ls\" without a symbolic link", static_cast<unsigned>(i),
                info.name.c_str());
            continue;
        }
        const bool duplicate = std::any_of(devices.begin(), devices.end(), [&](const VideoDeviceInfo& other) {
            return EqualsNoCase(other.symbolicLink, info.symbolicLink);
        });
        if (duplicate) continue;
        if (info.name.empty()) info.name = L"비디오 장치";

        // The link of a USB camera carries its ids ("\\?\usb#vid_07ca&pid_3112&mi_00#...");
        // otherwise ask the PnP tree, which also has the container id shared with its audio.
        info.model = UsbIdsFromPath(info.symbolicLink);
        std::wstring instanceId;
        if (auto facts = FactsForInterface(info.symbolicLink)) {
            instanceId = facts->instanceId;
            info.containerId = facts->containerId;
            if (info.model.empty()) info.model = facts->model;
        }

        const bool nameSaysVirtual = ContainsNoCase(info.name, L"virtual") || info.name.find(L"OBS") != std::wstring::npos;
        const bool hasUsbId = ContainsNoCase(info.symbolicLink, L"vid_") || !info.model.empty();
        const bool isPci = ContainsNoCase(info.symbolicLink, L"pci#");
        info.isVirtual = nameSaysVirtual || (!hasUsbId && !isPci);
        // Virtual cameras are never auto-selected, so don't start their producer just to look.
        info.looksLikeCaptureCard = !info.isVirtual && LooksLikeCaptureCard(activate, info);

        Log(L"Devices: video %u \"%ls\" model=%ls container=%ls virtual=%d captureCard=%d link=%ls instance=%ls",
            static_cast<unsigned>(devices.size()), info.name.c_str(), OrDash(info.model), OrDash(info.containerId),
            info.isVirtual ? 1 : 0, info.looksLikeCaptureCard ? 1 : 0, info.symbolicLink.c_str(), OrDash(instanceId));
        devices.push_back(std::move(info));
    }

    Log(L"Devices: %u video device(s) of %u source(s) in %llu ms", static_cast<unsigned>(devices.size()),
        static_cast<unsigned>(activates.size()), static_cast<unsigned long long>(GetTickCount64() - started));
    return devices;
}

std::vector<AudioInputInfo> EnumerateAudioInputs() {
    std::vector<AudioInputInfo> inputs;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(LogIfFailed(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)),
                           L"Devices: create MMDeviceEnumerator"))) {
        return inputs;
    }
    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(LogIfFailed(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection),
                           L"Devices: EnumAudioEndpoints(eCapture)")) ||
        !collection) {
        return inputs;
    }
    UINT count = 0;
    if (FAILED(LogIfFailed(collection->GetCount(&count), L"Devices: IMMDeviceCollection::GetCount"))) return inputs;

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(LogIfFailed(collection->Item(i, &device), L"Devices: IMMDeviceCollection::Item")) || !device) continue;

        AudioInputInfo info;
        LPWSTR rawId = nullptr;
        if (FAILED(LogIfFailed(device->GetId(&rawId), L"Devices: IMMDevice::GetId")) || !rawId) continue;
        info.id = rawId;
        CoTaskMemFree(rawId);

        std::wstring endpointInstanceId;
        ComPtr<IPropertyStore> store;
        if (SUCCEEDED(LogIfFailed(device->OpenPropertyStore(STGM_READ, &store), L"Devices: OpenPropertyStore")) && store) {
            info.name = StringProperty(store.Get(), kKeyFriendlyName);
            info.deviceName = StringProperty(store.Get(), kKeyInterfaceFriendlyName);
            info.containerId = ContainerProperty(store.Get(), kKeyContainerId);
            endpointInstanceId = StringProperty(store.Get(), kKeyInstanceId);
        }
        if (info.name.empty()) info.name = info.deviceName.empty() ? info.id : info.deviceName;

        // USB VID:PID, most reliable first: the adapter the endpoint is wired to (its KS filter
        // path names the USB function), then the adapter's devnode, then the endpoint's devnode
        // (SWD\MMDEVAPI\{0.0.1.00000000}.{guid}, whose parent is the adapter on current Windows).
        const std::wstring adapterId = ConnectedAdapterId(device.Get());
        const wchar_t* modelSource = L"-";
        info.model = UsbIdsFromPath(adapterId);
        if (!info.model.empty()) modelSource = L"adapter path";

        std::optional<DevNodeFacts> adapterFacts;
        if (!adapterId.empty() && (info.model.empty() || info.containerId.empty())) {
            adapterFacts = FactsForInterface(InterfacePathFromTopologyId(adapterId));
            if (adapterFacts) {
                if (info.model.empty() && !adapterFacts->model.empty()) {
                    info.model = adapterFacts->model;
                    modelSource = L"adapter devnode";
                }
                if (info.containerId.empty()) info.containerId = adapterFacts->containerId;
            }
        }
        if (info.model.empty() || info.containerId.empty()) {
            if (endpointInstanceId.empty()) endpointInstanceId = L"SWD\\MMDEVAPI\\" + info.id;
            if (info.model.empty()) {
                info.model = UsbIdsFromPath(endpointInstanceId);
                if (!info.model.empty()) modelSource = L"endpoint instance id";
            }
            if (info.model.empty() || info.containerId.empty()) {
                if (auto facts = FactsForInstance(endpointInstanceId)) {
                    if (info.model.empty() && !facts->model.empty()) {
                        info.model = facts->model;
                        modelSource = L"endpoint devnode";
                    }
                    if (info.containerId.empty()) info.containerId = facts->containerId;
                }
            }
        }

        Log(L"Devices: audio %u \"%ls\" adapter=\"%ls\" model=%ls (%ls) container=%ls id=%ls adapterId=%ls",
            static_cast<unsigned>(inputs.size()), info.name.c_str(), info.deviceName.c_str(), OrDash(info.model),
            modelSource, OrDash(info.containerId), info.id.c_str(), OrDash(adapterId));
        inputs.push_back(std::move(info));
    }

    Log(L"Devices: %u audio input(s)", static_cast<unsigned>(inputs.size()));
    return inputs;
}

namespace {

/// The App asks for the pairing every few seconds; log it only when the answer changes.
void LogPairing(const std::wstring& line) {
    static std::wstring last;  // UI thread only
    if (line == last) return;
    last = line;
    Log(L"%ls", line.c_str());
}

}  // namespace

std::optional<size_t> PairAudioInput(const VideoDeviceInfo& video, const std::vector<AudioInputInfo>& inputs) {
    auto nameMatches = [&](const AudioInputInfo& input) {
        return !video.name.empty() && (EqualsNoCase(input.deviceName, video.name) || ContainsNoCase(input.name, video.name));
    };
    // Among several candidates of one physical device (e.g. HDMI and line-in endpoints) prefer
    // the one named after the video device, else the first.
    auto pick = [&](auto&& sameDevice, const wchar_t* how) -> std::optional<size_t> {
        std::optional<size_t> first;
        for (size_t i = 0; i < inputs.size(); ++i) {
            if (!sameDevice(inputs[i])) continue;
            if (nameMatches(inputs[i])) {
                first = i;
                break;
            }
            if (!first) first = i;
        }
        if (first) {
            LogPairing(std::format(L"Devices: \"{}\" pairs with audio \"{}\" by {}", video.name, inputs[*first].name, how));
        }
        return first;
    };

    if (!video.containerId.empty()) {
        if (auto match = pick([&](const AudioInputInfo& input) { return EqualsNoCase(input.containerId, video.containerId); },
                              L"container id")) {
            return match;
        }
    }
    if (!video.model.empty()) {
        if (auto match = pick([&](const AudioInputInfo& input) { return EqualsNoCase(input.model, video.model); },
                              L"USB VID:PID")) {
            return match;
        }
    }
    if (auto match = pick(nameMatches, L"name")) return match;

    LogPairing(std::format(L"Devices: no audio input pairs with \"{}\" (model={} container={}, {} inputs)", video.name,
                           OrDash(video.model), OrDash(video.containerId), inputs.size()));
    return std::nullopt;
}
