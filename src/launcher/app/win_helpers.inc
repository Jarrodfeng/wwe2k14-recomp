// Writes the back buffer (RGBA8) as a 32-bit BMP.
bool SaveBackbuffer(const fs::path& out) {
  ID3D11Texture2D* back = nullptr;
  if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return false;
  D3D11_TEXTURE2D_DESC d;
  back->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING, d.BindFlags = 0, d.CPUAccessFlags = D3D11_CPU_ACCESS_READ, d.MiscFlags = 0;
  ID3D11Texture2D* staging = nullptr;
  bool ok = false;
  if (SUCCEEDED(g_device->CreateTexture2D(&d, nullptr, &staging))) {
    g_context->CopyResource(staging, back);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_context->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
      std::error_code ec;
      fs::create_directories(out.parent_path(), ec);
      std::ofstream f(out, std::ios::binary);
      const uint32_t w = d.Width, h = d.Height, size = w * h * 4;
      BITMAPFILEHEADER fh{0x4D42, DWORD(54 + size), 0, 0, 54};
      BITMAPINFOHEADER ih{sizeof(ih), LONG(w), -LONG(h), 1, 32, BI_RGB, size};
      f.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
      f.write(reinterpret_cast<const char*>(&ih), sizeof(ih));
      std::vector<uint8_t> row(w * 4);
      for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
        for (uint32_t x = 0; x < w; ++x)
          row[4 * x] = src[4 * x + 2], row[4 * x + 1] = src[4 * x + 1], row[4 * x + 2] = src[4 * x],
          row[4 * x + 3] = 255;
        f.write(reinterpret_cast<const char*>(row.data()), row.size());
      }
      ok = bool(f);
      g_context->Unmap(staging, 0);
    }
    staging->Release();
  }
  back->Release();
  return ok;
}

ID3D11ShaderResourceView* MakeTexture(const std::vector<uint8_t>& rgba, int w, int h) {
  if (rgba.empty() || w <= 0 || h <= 0) return nullptr;
  D3D11_TEXTURE2D_DESC d{};
  d.Width = UINT(w), d.Height = UINT(h), d.MipLevels = 1, d.ArraySize = 1, d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  d.SampleDesc.Count = 1, d.Usage = D3D11_USAGE_DEFAULT, d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  D3D11_SUBRESOURCE_DATA init{rgba.data(), UINT(w * 4), 0};
  ID3D11Texture2D* tex = nullptr;
  if (FAILED(g_device->CreateTexture2D(&d, &init, &tex))) return nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  g_device->CreateShaderResourceView(tex, nullptr, &srv);
  tex->Release();
  return srv;
}

std::optional<fs::path> PickFile(HWND owner) {
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return std::nullopt;
  COMDLG_FILTERSPEC types[] = {{L"Videos (*.mp4, *.mov, *.mkv, *.bik2)", L"*.mp4;*.mov;*.mkv;*.bik2"},
                               {L"All files", L"*.*"}};
  dlg->SetFileTypes(2, types);
  dlg->SetTitle(L"Pick a video for this tron");
  std::optional<fs::path> out;
  if (SUCCEEDED(dlg->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) out = fs::path(p), CoTaskMemFree(p);
      item->Release();
    }
  }
  dlg->Release();
  return out;
}

// Several audio files at once (Add a song...).
std::vector<fs::path> PickAudios(HWND owner) {
  std::vector<fs::path> out;
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return out;
  COMDLG_FILTERSPEC types[] = {{L"Music (mp3, wav, ogg, opus, flac, m4a, aac, wma)", L"*.mp3;*.wav;*.ogg;*.oga;*.opus;*.flac;*.m4a;*.aac;*.wma;*.mp4"},
                               {L"All files", L"*.*"}};
  dlg->SetFileTypes(2, types);
  dlg->SetTitle(L"Add songs to the game's MUSIC list");
  DWORD opts = 0;
  dlg->GetOptions(&opts);
  dlg->SetOptions(opts | FOS_ALLOWMULTISELECT);
  if (SUCCEEDED(dlg->Show(owner))) {
    IShellItemArray* arr = nullptr;
    if (SUCCEEDED(dlg->GetResults(&arr)) && arr) {
      DWORD n = 0;
      arr->GetCount(&n);
      for (DWORD i = 0; i < n; ++i) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(arr->GetItemAt(i, &item)) && item) {
          PWSTR p = nullptr;
          if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) out.emplace_back(p), CoTaskMemFree(p);
          item->Release();
        }
      }
      arr->Release();
    }
  }
  dlg->Release();
  return out;
}

// Session 33: an audio file for a wrestler's entrance music.
std::optional<fs::path> PickAudio(HWND owner) {
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return std::nullopt;
  COMDLG_FILTERSPEC types[] = {{L"Music (mp3, wav, ogg, opus, flac, m4a, aac, wma)", L"*.mp3;*.wav;*.ogg;*.oga;*.opus;*.flac;*.m4a;*.aac;*.wma;*.mp4"},
                               {L"All files", L"*.*"}};
  dlg->SetFileTypes(2, types);
  dlg->SetTitle(L"Pick this wrestler's entrance music");
  std::optional<fs::path> out;
  if (SUCCEEDED(dlg->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) out = fs::path(p), CoTaskMemFree(p);
      item->Release();
    }
  }
  dlg->Release();
  return out;
}

// Session 25: PNGs for the Logos page (several at once).
std::vector<fs::path> PickPngs(HWND owner) {
  std::vector<fs::path> out;
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return out;
  COMDLG_FILTERSPEC types[] = {{L"PNG pictures (*.png)", L"*.png"}};
  dlg->SetFileTypes(1, types);
  dlg->SetTitle(L"Pick PNG pictures to import as logos");
  FILEOPENDIALOGOPTIONS o = 0;
  dlg->GetOptions(&o);
  dlg->SetOptions(o | FOS_ALLOWMULTISELECT | FOS_FILEMUSTEXIST);
  if (SUCCEEDED(dlg->Show(owner))) {
    IShellItemArray* items = nullptr;
    if (SUCCEEDED(dlg->GetResults(&items))) {
      DWORD n = 0;
      items->GetCount(&n);
      for (DWORD i = 0; i < n; ++i) {
        IShellItem* item = nullptr;
        if (FAILED(items->GetItemAt(i, &item))) continue;
        PWSTR path = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) out.emplace_back(path), CoTaskMemFree(path);
        item->Release();
      }
      items->Release();
    }
  }
  dlg->Release();
  return out;
}

// Session 26: a disc image of the player's own disc as the game files.
std::optional<fs::path> PickDiscImage(HWND owner) {
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return std::nullopt;
  COMDLG_FILTERSPEC types[] = {{L"Xbox 360 disc images (*.iso)", L"*.iso"}, {L"All files", L"*.*"}};
  dlg->SetFileTypes(2, types);
  dlg->SetTitle(L"Pick the disc image (.iso) of your WWE 2K14 disc");
  std::optional<fs::path> out;
  if (SUCCEEDED(dlg->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) out = fs::path(p), CoTaskMemFree(p);
      item->Release();
    }
  }
  dlg->Release();
  return out;
}

// Session 98 (EXPORT FOR PHONE): where the tron previews zip goes. Starts on the Desktop with wwe2k14_tron_previews.zip.
std::optional<fs::path> PickSaveZip(HWND owner, const wchar_t* title, const wchar_t* name) {
  IFileSaveDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return std::nullopt;
  COMDLG_FILTERSPEC types[] = {{L"Zip files (*.zip)", L"*.zip"}};
  dlg->SetFileTypes(1, types);
  dlg->SetDefaultExtension(L"zip");
  dlg->SetTitle(title);
  dlg->SetFileName(name);
  PWSTR desk = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk))) {
    IShellItem* folder = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(desk, nullptr, IID_PPV_ARGS(&folder)))) dlg->SetFolder(folder), folder->Release();
    CoTaskMemFree(desk);
  }
  FILEOPENDIALOGOPTIONS o = 0;
  dlg->GetOptions(&o);
  dlg->SetOptions(o | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM);
  std::optional<fs::path> out;
  if (SUCCEEDED(dlg->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) out = fs::path(p), CoTaskMemFree(p);
      item->Release();
    }
  }
  dlg->Release();
  return out;
}

std::optional<fs::path> PickFolder(HWND owner, const wchar_t* title) {
  IFileOpenDialog* dlg = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return std::nullopt;
  FILEOPENDIALOGOPTIONS o = 0;
  dlg->GetOptions(&o);
  dlg->SetOptions(o | FOS_PICKFOLDERS);
  dlg->SetTitle(title);
  std::optional<fs::path> out;
  if (SUCCEEDED(dlg->Show(owner))) {
    IShellItem* item = nullptr;
    if (SUCCEEDED(dlg->GetResult(&item))) {
      PWSTR p = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) out = fs::path(p), CoTaskMemFree(p);
      item->Release();
    }
  }
  dlg->Release();
  return out;
}
