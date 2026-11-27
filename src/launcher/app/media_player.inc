
// ---------------------------------------------------------------------------------------------------------------

// Stacked alpha (Session 18): `px` holds the colour half (4 bytes a pixel), `alpha_half` the bottom half; each pixel's
// alpha becomes the bottom half's grey (its green channel; the decoder already expanded it to full range).
inline void MergeStackedAlpha(uint8_t* px, const uint8_t* alpha_half, size_t pixels) {
  for (size_t i = 0; i < pixels; ++i) px[4 * i + 3] = alpha_half[4 * i + 1];
}

// The same for a whole RGBA picture (w x h, h = 2 x the picture): keeps the top half with the alpha merged in.
inline void UnstackRgba(std::vector<uint8_t>& rgba, int w, int& h) {
  if (h < 4 || rgba.size() < size_t(w) * h * 4) return;
  const int ph = h / 2;
  MergeStackedAlpha(rgba.data(), rgba.data() + size_t(w) * (h - ph) * 4, size_t(w) * ph);
  rgba.resize(size_t(w) * ph * 4);
  h = ph;
}

struct Thumb {
  fs::path file;
  fs::file_time_type stamp;
  VideoInfo info;
  ID3D11ShaderResourceView* srv = nullptr;
  int w = 0, h = 0;
  ~Thumb() { ReleaseLater(srv); }  // may be reset mid-frame (Assign / Revert)
};

// Plays one video into a texture (the hovered tile's preview). A background thread decodes ahead (up to 3 frames);
// the UI thread only shows the frame that is due (time stamps on the wall clock) and loops at the end. With the GPU
// path the frame is copied GPU-to-GPU into our texture; otherwise (no video support, or a format the GPU path refuses)
// the thread decodes in software and the UI uploads the pixels.
class Player {
 public:
  ~Player() { Close(); }
  const fs::path& File() const { return file_; }
  int W() const { return w_; }
  int H() const { return h_; }
  bool Playing() const { return running_; }
  bool Gpu() const { return gpu_; }

  bool Open(const fs::path& f) {
    Close();
    file_ = f;
    VideoInfo probe = MfInfo(f, 0.0, nullptr);  // size (and starts Media Foundation)
    if (!probe.ok) return false;
    stacked_ = IsStackedAlpha(f) && probe.h >= 4;  // Session 18: colour on top, alpha below -> one RGBA picture
    w_ = probe.w, h_ = stacked_ ? probe.h / 2 : probe.h, full_h_ = probe.h;
    stop_ = false, running_ = true;
    start_set_ = false, have_frame_ = false;
    thread_ = std::thread([this, f] { wwe2k14::NameThread(L"lnch_decode"); Decode(f); });
    return true;
  }

  void Close() {
    if (thread_.joinable()) {
      {
        std::lock_guard<std::mutex> l(mu_);
        stop_ = true;
      }
      cv_.notify_all();
      thread_.join();
    }
    for (auto& fr : queue_) fr.Free();
    queue_.clear();
    ReleaseLater(srv_), srv_ = nullptr;
    ReleaseLater(tex_), tex_ = nullptr;
    file_.clear();
    running_ = false, have_frame_ = false, gpu_ = false;
  }

  // Shows the frame that is due now; returns the texture once it holds a frame.
  ID3D11ShaderResourceView* Tick() {
    if (!running_) return nullptr;
    const auto now = std::chrono::steady_clock::now();
    Frame show;
    bool got = false;
    {
      std::lock_guard<std::mutex> l(mu_);
      while (!queue_.empty()) {
        Frame& front = queue_.front();
        if (front.loop) {  // the video ended and restarted: its time stamps start again
          if (got) break;  // show the last frame first
          start_set_ = false;
          queue_.pop_front();
          continue;
        }
        if (!start_set_) start_ = now - std::chrono::microseconds(front.ts / 10), start_set_ = true;
        const LONGLONG elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - start_).count() * 10;
        if (front.ts > elapsed && (got || have_frame_)) break;  // not due yet
        if (got) show.Free();
        show = std::move(front), got = true;
        queue_.pop_front();
      }
    }
    cv_.notify_all();  // room in the queue
    if (got) {
      Present(show);
      show.Free();
    }
    return have_frame_ ? srv_ : nullptr;
  }

 private:
  struct Frame {
    LONGLONG ts = 0;
    bool loop = false;
    IMFSample* sample = nullptr;  // GPU path: the decoder's texture lives in the sample
    std::vector<uint8_t> px;      // software path: BGRA rows, w_ * 4 apart
    void Free() {
      if (sample) sample->Release(), sample = nullptr;
      px.clear();
    }
    Frame() = default;
    Frame(Frame&& o) noexcept : ts(o.ts), loop(o.loop), sample(o.sample), px(std::move(o.px)) { o.sample = nullptr; }
    Frame& operator=(Frame&& o) noexcept {
      if (this != &o) Free(), ts = o.ts, loop = o.loop, sample = o.sample, px = std::move(o.px), o.sample = nullptr;
      return *this;
    }
  };

  bool EnsureTexture(DXGI_FORMAT fmt) {
    if (tex_ && fmt_ == fmt) return true;
    ReleaseLater(srv_), srv_ = nullptr;
    ReleaseLater(tex_), tex_ = nullptr;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = UINT(w_), d.Height = UINT(h_), d.MipLevels = 1, d.ArraySize = 1, d.Format = fmt;
    d.SampleDesc.Count = 1, d.Usage = D3D11_USAGE_DEFAULT, d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_device->CreateTexture2D(&d, nullptr, &tex_))) return false;
    fmt_ = fmt;
    return SUCCEEDED(g_device->CreateShaderResourceView(tex_, nullptr, &srv_));
  }

  void Present(Frame& f) {
    if (f.sample) {  // GPU: copy the decoder's texture into ours
      IMFMediaBuffer* b = nullptr;
      IMFDXGIBuffer* db = nullptr;
      ID3D11Texture2D* src = nullptr;
      UINT sub = 0;
      if (SUCCEEDED(f.sample->GetBufferByIndex(0, &b)) && SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&db))) &&
          SUCCEEDED(db->GetResource(IID_PPV_ARGS(&src))) && SUCCEEDED(db->GetSubresourceIndex(&sub))) {
        D3D11_TEXTURE2D_DESC sd;
        src->GetDesc(&sd);
        if (EnsureTexture(sd.Format)) {
          const D3D11_BOX box{0, 0, 0, UINT(std::min<int>(w_, int(sd.Width))), UINT(std::min<int>(h_, int(sd.Height))), 1};
          g_context->CopySubresourceRegion(tex_, 0, 0, 0, 0, src, sub, &box);
          have_frame_ = true;
          LogPreviewPixel(tex_, f.ts, true, w_, h_);
        }
      }
      if (src) src->Release();
      if (db) db->Release();
      if (b) b->Release();
    } else if (!f.px.empty() && EnsureTexture(DXGI_FORMAT_B8G8R8A8_UNORM)) {  // software: upload the pixels
      g_context->UpdateSubresource(tex_, 0, nullptr, f.px.data(), UINT(w_) * 4, 0);
      have_frame_ = true;
      LogPreviewPixel(tex_, f.ts, false, w_, h_);
    }
  }

  IMFSourceReader* MakeReader(const fs::path& f, bool gpu) {
    IMFAttributes* a = nullptr;
    MFCreateAttributes(&a, 3);
    if (gpu) {
      a->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, g_dxgi_manager);
      a->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
      a->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    } else {
      a->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    }
    IMFSourceReader* r = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(f.wstring().c_str(), a, &r);
    a->Release();
    if (FAILED(hr)) return nullptr;
    r->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    r->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    IMFMediaType* t = nullptr;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    // GPU: B8G8R8A8 (opaque: alpha 255). Software: RGB32 (the basic converter has no ARGB32; Session 18), alpha set by us.
    t->SetGUID(MF_MT_SUBTYPE, gpu ? MFVideoFormat_ARGB32 : MFVideoFormat_RGB32);
    hr = r->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, t);
    t->Release();
    if (FAILED(hr)) return r->Release(), nullptr;
    return r;
  }

  // The decode thread: reads ahead, keeps at most 3 frames queued, rewinds at the end.
  void Decode(fs::path f) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMFSourceReader* r = g_dxgi_manager && !stacked_ ? MakeReader(f, true) : nullptr;  // stacked: CPU (merge)
    gpu_ = r != nullptr;
    if (!r) r = MakeReader(f, false);
    if (!r) {
      running_ = false;
      CoUninitialize();
      return;
    }
    for (;;) {
      {
        std::unique_lock<std::mutex> l(mu_);
        cv_.wait(l, [this] { return stop_ || queue_.size() < 3; });
        if (stop_) break;
      }
      DWORD flags = 0;
      LONGLONG ts = 0;
      IMFSample* s = nullptr;
      if (FAILED(r->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &s))) break;
      Frame fr;
      if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
        PROPVARIANT pos;
        PropVariantInit(&pos);
        pos.vt = VT_I8;
        pos.hVal.QuadPart = 0;
        r->SetCurrentPosition(GUID_NULL, pos);
        fr.loop = true;
      } else if (!s) {
        continue;
      } else {
        fr.ts = ts;
        IMFMediaBuffer* b = nullptr;
        IMFDXGIBuffer* db = nullptr;
        if (gpu_ && SUCCEEDED(s->GetBufferByIndex(0, &b)) && SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&db)))) {
          fr.sample = s, s = nullptr;  // the UI copies it (GPU-to-GPU)
        } else {
          IMFMediaBuffer* cb = nullptr;  // software path: BGRA bytes for the UI to upload
          if (SUCCEEDED(s->ConvertToContiguousBuffer(&cb))) {
            BYTE* data = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(cb->Lock(&data, nullptr, &len))) {
              if (stacked_ && len >= DWORD(w_ * full_h_ * 4)) {
                fr.px.assign(data, data + size_t(w_) * h_ * 4);
                MergeStackedAlpha(fr.px.data(), data + size_t(w_) * (full_h_ - h_) * 4, size_t(w_) * h_);
              } else if (len >= DWORD(w_ * h_ * 4)) {
                fr.px.assign(data, data + size_t(w_) * h_ * 4);
                for (size_t i = 3; i < fr.px.size(); i += 4) fr.px[i] = 255;  // RGB32's fourth byte is undefined
              }
              cb->Unlock();
            }
            cb->Release();
          }
        }
        if (db) db->Release();
        if (b) b->Release();
        if (s) s->Release();
      }
      {
        std::lock_guard<std::mutex> l(mu_);
        queue_.push_back(std::move(fr));
      }
      WakeUi();
    }
    r->Release();
    CoUninitialize();
  }

  fs::path file_;
  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Frame> queue_;
  bool stop_ = false;
  std::atomic<bool> running_{false}, gpu_{false};
  ID3D11Texture2D* tex_ = nullptr;
  ID3D11ShaderResourceView* srv_ = nullptr;
  DXGI_FORMAT fmt_ = DXGI_FORMAT_UNKNOWN;
  int w_ = 0, h_ = 0, full_h_ = 0;
  bool stacked_ = false;
  std::chrono::steady_clock::time_point start_;
  bool start_set_ = false, have_frame_ = false;
};

// Thumbnails decoded on a worker thread (PNG with stb_image; the first frame of an assigned video with Media
// Foundation); the UI thread only turns finished pictures into textures.
class ThumbLoader {
 public:
  struct Done {
    int tron = 0;
    fs::path file;
    fs::file_time_type stamp;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
  };
  ~ThumbLoader() {
    {
      std::lock_guard<std::mutex> l(mu_);
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }
  void Request(int tron, const fs::path& f, fs::file_time_type stamp) {
    std::lock_guard<std::mutex> l(mu_);
    if (!thread_.joinable()) thread_ = std::thread([this] { wwe2k14::NameThread(L"lnch_run"); Run(); });
    for (const auto& q : todo_)
      if (q.tron == tron && q.file == f) return;
    todo_.push_back({tron, f, stamp, {}, 0, 0});
    cv_.notify_all();
  }
  std::vector<Done> TakeDone() {
    std::lock_guard<std::mutex> l(mu_);
    std::vector<Done> out;
    out.swap(done_);
    return out;
  }
  bool Busy() {
    std::lock_guard<std::mutex> l(mu_);
    return !todo_.empty() || working_;
  }

 private:
  void Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
      Done job;
      {
        std::unique_lock<std::mutex> l(mu_);
        cv_.wait(l, [this] { return stop_ || !todo_.empty(); });
        if (stop_) break;
        job = std::move(todo_.front());
        todo_.pop_front();
        working_ = true;
      }
      if (Lower(PathStr(job.file.extension())) == ".png") {
        int n = 0;
        if (FILE* fp = _wfopen(job.file.c_str(), L"rb")) {
          if (unsigned char* px = stbi_load_from_file(fp, &job.w, &job.h, &n, 4)) {
            job.rgba.assign(px, px + size_t(job.w) * job.h * 4);
            stbi_image_free(px);
          }
          std::fclose(fp);
        }
      } else {
        VideoInfo vi = MfInfo(job.file, 0.0, &job.rgba);
        job.w = vi.w, job.h = vi.h;
        if (IsStackedAlpha(job.file)) UnstackRgba(job.rgba, job.w, job.h);
      }
      {
        std::lock_guard<std::mutex> l(mu_);
        done_.push_back(std::move(job));
        working_ = false;
      }
      WakeUi();
    }
    CoUninitialize();
  }
  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Done> todo_;
  std::vector<Done> done_;
  bool stop_ = false, working_ = false;
};

struct Step {
  std::string op, a, b;
};
