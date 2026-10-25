#include <AYAudio/OutputDiagnostics.h>
#ifdef _WIN32
#include <Windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <algorithm>
#include <cmath>
#include <memory>

namespace ayt::audio {
namespace {
template<class T>struct Com {T* value=nullptr;~Com(){if(value)value->Release();}T** put(){return &value;}T* operator->()const{return value;}};
struct Apartment{HRESULT result=CoInitializeEx(nullptr,COINIT_MULTITHREADED);~Apartment(){if(SUCCEEDED(result))CoUninitialize();}};
struct TaskString{LPWSTR value=nullptr;~TaskString(){CoTaskMemFree(value);}};
std::string utf8(const wchar_t* text){
    if(!text)return {};const auto length=std::min<std::size_t>(wcslen(text),512);const auto bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,static_cast<int>(length),nullptr,0,nullptr,nullptr);if(bytes<=0)return {};std::string out(bytes,'\0');WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,static_cast<int>(length),out.data(),bytes,nullptr,nullptr);return out;
}
std::wstring identity(IMMDevice* device){TaskString id;if(!device||FAILED(device->GetId(&id.value))||!id.value)return {};return id.value;}
std::string name(IMMDevice* device){
    Com<IPropertyStore> properties;if(!device||FAILED(device->OpenPropertyStore(STGM_READ,properties.put())))return {};
    PROPVARIANT value;PropVariantInit(&value);std::string result;if(SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName,&value))&&value.vt==VT_LPWSTR)result=utf8(value.pwszVal);PropVariantClear(&value);return result;
}
bool valid(float value){return std::isfinite(value)&&value>=0&&value<=1;}
OutputVolume endpointVolume(IMMDevice* device){
    OutputVolume result;Com<IAudioEndpointVolume> volume;if(!device||FAILED(device->Activate(__uuidof(IAudioEndpointVolume),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(volume.put()))))return result;
    BOOL muted=FALSE;float master=0;UINT count=0;if(FAILED(volume->GetMute(&muted))||FAILED(volume->GetMasterVolumeLevelScalar(&master))||!valid(master))return result;
    result.known=true;result.muted=muted!=FALSE;result.master=master;
    if(SUCCEEDED(volume->GetChannelCount(&count))&&count<=32)for(UINT i=0;i<count;++i){float channel=0;if(FAILED(volume->GetChannelVolumeLevelScalar(i,&channel))||!valid(channel)){result.channels.clear();break;}result.channels.push_back(channel);}
    return result;
}
OutputEndpoint defaultEndpoint(IMMDeviceEnumerator* devices,ERole role,std::wstring& id){
    OutputEndpoint result;Com<IMMDevice> device;if(FAILED(devices->GetDefaultAudioEndpoint(eRender,role,device.put())))return result;
    id=identity(device.value);result.available=!id.empty();result.name=name(device.value);result.volume=endpointVolume(device.value);return result;
}
OutputVolume sessionVolume(IAudioSessionControl* session){
    OutputVolume result;Com<ISimpleAudioVolume> simple;if(FAILED(session->QueryInterface(__uuidof(ISimpleAudioVolume),reinterpret_cast<void**>(simple.put()))))return result;
    BOOL muted=FALSE;float master=0;if(FAILED(simple->GetMute(&muted))||FAILED(simple->GetMasterVolume(&master))||!valid(master))return result;
    result.known=true;result.muted=muted!=FALSE;result.master=master;
    Com<IChannelAudioVolume> channels;UINT count=0;
    if(SUCCEEDED(session->QueryInterface(__uuidof(IChannelAudioVolume),reinterpret_cast<void**>(channels.put())))&&SUCCEEDED(channels->GetChannelCount(&count))&&count<=32){result.channels.resize(count);if(count&&FAILED(channels->GetAllVolumes(count,result.channels.data())))result.channels.clear();if(!std::all_of(result.channels.begin(),result.channels.end(),valid))result.channels.clear();}
    return result;
}
}
OutputDiagnostics queryProcessOutput(){
    OutputDiagnostics result;result.status=OutputQueryStatus::Unavailable;Apartment apartment;if(FAILED(apartment.result)&&apartment.result!=RPC_E_CHANGED_MODE)return result;
    Com<IMMDeviceEnumerator> devices;if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(devices.put()))))return result;
    std::wstring consoleId,multimediaId;result.console=defaultEndpoint(devices.value,eConsole,consoleId);result.multimedia=defaultEndpoint(devices.value,eMultimedia,multimediaId);
    Com<IMMDeviceCollection> endpoints;if(FAILED(devices->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,endpoints.put())))return result;
    UINT count=0;if(FAILED(endpoints->GetCount(&count)))return result;result.status=OutputQueryStatus::Ok;
    if(count>32){count=32;result.truncated=true;}unsigned scanned=0;
    for(UINT i=0;i<count&&scanned<512&&result.processSessions.size()<32;++i){
        Com<IMMDevice> device;Com<IAudioSessionManager2> manager;Com<IAudioSessionEnumerator> sessions;
        if(FAILED(endpoints->Item(i,device.put()))||FAILED(device->Activate(__uuidof(IAudioSessionManager2),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(manager.put())))||FAILED(manager->GetSessionEnumerator(sessions.put()))){result.status=OutputQueryStatus::Partial;continue;}
        int size=0;if(FAILED(sessions->GetCount(&size))||size<0){result.status=OutputQueryStatus::Partial;continue;}if(size>128){size=128;result.truncated=true;}
        const auto id=identity(device.value);
        for(int j=0;j<size&&scanned<512&&result.processSessions.size()<32;++j){
            ++scanned;Com<IAudioSessionControl> control;Com<IAudioSessionControl2> identity;DWORD pid=0;
            if(FAILED(sessions->GetSession(j,control.put()))||FAILED(control->QueryInterface(__uuidof(IAudioSessionControl2),reinterpret_cast<void**>(identity.put()))))continue;
            // Non-S_OK can identify only the creator of a multi-process session.
            // Do not attribute shared or unknown sessions to this process.
            if(identity->GetProcessId(&pid)!=S_OK||pid!=GetCurrentProcessId())continue;
            ProcessOutputSession observed;observed.endpointName=name(device.value);observed.defaultForConsole=!id.empty()&&id==consoleId;observed.defaultForMultimedia=!id.empty()&&id==multimediaId;
            AudioSessionState state;if(SUCCEEDED(control->GetState(&state)))observed.state=state==AudioSessionStateActive?OutputSessionState::Active:state==AudioSessionStateInactive?OutputSessionState::Inactive:OutputSessionState::Expired;
            observed.session=sessionVolume(control.value);observed.endpoint=endpointVolume(device.value);result.processSessions.push_back(std::move(observed));
        }
    }
    if(scanned>=512||result.processSessions.size()>=32)result.truncated=true;
    if(!result.console.available&&!result.multimedia.available||result.truncated)result.status=OutputQueryStatus::Partial;
    return result;
}
}
#else
namespace ayt::audio { OutputDiagnostics queryProcessOutput(){return {};} }
#endif
