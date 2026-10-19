// Session 33 (item 5 check): record what the default playback device plays (WASAPI loopback) to a WAV file, for N
// seconds. Hears everything the PC plays: the game's own sound and anything else (custom entrance music played by
// Media Foundation, other programs).
//   loopback_rec.exe OUT.wav SECONDS [DEVICE]   DEVICE = part of a playback device's name (default: the default one;
//                                               the game logs its endpoint: "audio endpoint 'Main Output 1/2 ...'")
// Build: clang-cl /O2 /EHsc tools\audio\loopback_rec.cpp /Fe:out\tools\loopback_rec.exe ole32.lib
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propsys.h>

// PKEY_Device_FriendlyName (functiondiscoverykeys_devpkey.h), defined here so no GUID library is needed.
static const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#pragma comment(lib, "ole32.lib")

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) {
    std::fwprintf(stderr, L"usage: loopback_rec OUT.wav SECONDS\n");
    return 2;
  }
  const double seconds = _wtof(argv[2]);
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator* en = nullptr;
  IMMDevice* dev = nullptr;
  IAudioClient* client = nullptr;
  IAudioCaptureClient* cap = nullptr;
  WAVEFORMATEX* fmt = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return 1;
  if (argc > 3) {
    IMMDeviceCollection* all = nullptr;
    UINT n = 0;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all)) && SUCCEEDED(all->GetCount(&n))) {
      for (UINT i = 0; i < n && !dev; ++i) {
        IMMDevice* d = nullptr;
        IPropertyStore* ps = nullptr;
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(all->Item(i, &d)) && SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) &&
            SUCCEEDED(ps->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR && wcsstr(v.pwszVal, argv[3])) {
          std::fwprintf(stdout, L"loopback_rec: device %s\n", v.pwszVal);
          dev = d, d = nullptr;
        }
        PropVariantClear(&v);
        if (ps) ps->Release();
        if (d) d->Release();
      }
    }
    if (!dev) {
      std::fwprintf(stderr, L"loopback_rec: no active playback device named like '%s'\n", argv[3]);
      return 1;
    }
  }
  if ((!dev && FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) ||
      FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client))) ||
      FAILED(client->GetMixFormat(&fmt)) ||
      FAILED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 10000000, 0, fmt, nullptr)) ||
      FAILED(client->GetService(IID_PPV_ARGS(&cap))) || FAILED(client->Start())) {
    std::fwprintf(stderr, L"loopback_rec: could not open the playback device for loopback\n");
    return 1;
  }
  const UINT32 frame_bytes = fmt->nBlockAlign;
  std::vector<BYTE> data;
  const ULONGLONG want = ULONGLONG(seconds * fmt->nSamplesPerSec);
  ULONGLONG frames = 0;
  const DWORD start = GetTickCount();
  while (frames < want && GetTickCount() - start < DWORD(seconds * 1000) + 3000) {
    Sleep(10);
    UINT32 packet = 0;
    while (SUCCEEDED(cap->GetNextPacketSize(&packet)) && packet) {
      BYTE* buf = nullptr;
      UINT32 n = 0;
      DWORD flags = 0;
      if (FAILED(cap->GetBuffer(&buf, &n, &flags, nullptr, nullptr))) break;
      const size_t at = data.size();
      data.resize(at + size_t(n) * frame_bytes);
      if (flags & AUDCLNT_BUFFERFLAGS_SILENT) std::memset(&data[at], 0, size_t(n) * frame_bytes);
      else std::memcpy(&data[at], buf, size_t(n) * frame_bytes);
      frames += n;
      cap->ReleaseBuffer(n);
    }
    // Loopback delivers no packets while nothing plays: pad with silence so the file keeps real time.
    const ULONGLONG due = ULONGLONG((GetTickCount() - start) / 1000.0 * fmt->nSamplesPerSec);
    if (due > frames + fmt->nSamplesPerSec / 5) {
      const ULONGLONG pad = due - frames;
      data.resize(data.size() + size_t(pad) * frame_bytes, 0);
      frames += pad;
    }
  }
  client->Stop();
  FILE* f = _wfopen(argv[1], L"wb");
  if (!f) return 1;
  const bool ext = fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE;
  const DWORD fmt_size = ext ? 40 : 16 + (fmt->cbSize ? 2 + fmt->cbSize : 0);
  const DWORD data_size = DWORD(data.size()), riff = 4 + 8 + fmt_size + 8 + data_size;
  std::fwrite("RIFF", 1, 4, f), std::fwrite(&riff, 4, 1, f), std::fwrite("WAVEfmt ", 1, 8, f);
  std::fwrite(&fmt_size, 4, 1, f), std::fwrite(fmt, 1, fmt_size, f);
  std::fwrite("data", 1, 4, f), std::fwrite(&data_size, 4, 1, f), std::fwrite(data.data(), 1, data.size(), f);
  std::fclose(f);
  std::fwprintf(stdout, L"loopback_rec: %llu frames at %u Hz, %u channels -> %s\n", frames, fmt->nSamplesPerSec,
                fmt->nChannels, argv[1]);
  return 0;
}
