// S105 SKB3 unit: the ranged XCTD reader (src\assets\xctd_ranged.h) and the disc evtobj.pac's ranged state (src\assets\asset_load.h EvtRanged / OpenEvtSlice / PropPachEvt).
// entry: wwe2k14::xctd_ranged::BuildIndex / DecodeRange, assets::load_detail::OpenEvtRanged / OpenEvtSlice, assets::PropPachEvt, assets::load_detail::ReleaseEvtobj
// Why the unit enters here: the production caller is GameFilesSource::Load (src\renderer_scene\live_assets.cpp:326 PropPachEvt, :333 LoadPropEntry, the loader thread's "evtprop:<id>/<k>" job) and MarkEvtobjReleasable -> ReleaseEvtobj (:210, BellRelease's retry);
// they are plain functions of assets:: (no hook, no guest state), called here in the production order: PropPachEvt (ordinal from the directory) -> OpenEvtSlice (inside LoadPropEntry) -> ReleaseEvtobj -> re-ask.
// Default mode: no game files. A synthetic XCTD pac is BUILT here (stored blocks, a 64 KB gap, PACHs spanning blocks, a directory whose row order differs from the file order, an id in both SOBJ and BOBJ),
// written under out\SKB3\fixture_root\pac\evt\evtobj.pac and read through the production path (GameFiles::SetRoot -> OpenEvtRanged -> PropPachEvt -> OpenEvtSlice). Nothing from the game is read or committed.
//   evtobj_ranged_unit.exe [--plant corrupt|skip_gap|drop_row|tail]    each plant must make it exit 1 and fail its named check
//   evtobj_ranged_unit.exe [--comp] [--plant ...]                      --comp: the same unit over a fixture of LZX blocks (uncompressed-type LZX blocks in real chunk headers, 2 chunks per block)
//   evtobj_ranged_unit.exe --real GAME_DIR --all                       S105 SKB3 fix M1 (the gate check): on the player's own file, every block decoded byte-equal to the whole unpack, every directory row / slice, every directory id (needs game_data; a missing file = FAIL)
//   evtobj_ranged_unit.exe --real GAME_DIR [--verify]                  offline timing on the player's own pac\evt\evtobj.pac: per-prop ms (index, ordinal, slice, LoadPropEntry); --verify = slice byte-equal to the whole open
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../src/assets/asset_load.h"

using namespace assets;
namespace fs = std::filesystem;
namespace xr = wwe2k14::xctd_ranged;

namespace {
int g_fail = 0;
std::string g_plant;
bool g_comp = false;   // S105 SKB3 fix M1: --comp = every block is an LZX block (an UNCOMPRESSED-type LZX block inside real chunk headers: the compressed branch of BuildIndex / DecodeRange, multi-chunk blocks), as the player's file is
void Check(const char* n, bool ok) {
  std::printf("%s  %s\n", ok ? "PASS" : "FAIL", n);
  if (!ok) ++g_fail;
}
double Ms(std::chrono::steady_clock::time_point a) { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count(); }

void Put32(Bytes& b, size_t o, uint32_t v) { std::memcpy(b.data() + o, &v, 4); }   // little-endian host
uint32_t Rng(uint32_t& s) { return s = s * 1664525u + 1013904223u; }

struct Fixture {
  Bytes unpacked, file;
  std::vector<size_t> pach_at;                 // byte start of each PACH, in file order
  std::map<int, int> sobj, bobj;               // prop id -> PACH ordinal (file order) by construction
  std::vector<std::vector<std::pair<uint32_t, Bytes>>> ents;   // per PACH: (entry id, bytes)
  size_t gap_block = 0;                        // the block that starts after a zero gap
};

Fixture Build() {
  Fixture f;
  const int kN = 12;
  Bytes& u = f.unpacked;
  u.assign(0x4000, 0);
  size_t pos = 0x4000;
  for (int p = 0; p < kN; ++p) {
    uint32_t seed = 1000u + uint32_t(p);
    std::vector<std::pair<uint32_t, Bytes>> es;
    const uint32_t sizes[3] = {20000u + uint32_t(p) * 7919u % 90000u, 5000u + uint32_t(p) * 311u, 1200u};
    const uint32_t ids[3] = {0, 10, 4};
    const int cnt = p % 3 == 2 ? 3 : 2;
    for (int e = 0; e < cnt; ++e) {
      Bytes d(sizes[e]);
      for (auto& c : d) c = uint8_t(Rng(seed) >> 24);
      es.push_back({ids[e], std::move(d)});
    }
    const size_t first = 8 + 12 * size_t(cnt);
    size_t total = first;
    for (auto& e : es) total += e.second.size();
    Bytes pc(total, 0);
    std::memcpy(pc.data(), "PACH", 4);
    Put32(pc, 4, uint32_t(cnt));
    size_t off = 0;
    for (int e = 0; e < cnt; ++e) {
      Put32(pc, 8 + 12 * size_t(e), es[size_t(e)].first), Put32(pc, 8 + 12 * size_t(e) + 4, uint32_t(off)), Put32(pc, 8 + 12 * size_t(e) + 8, uint32_t(es[size_t(e)].second.size()));
      std::memcpy(pc.data() + first + off, es[size_t(e)].second.data(), es[size_t(e)].second.size());
      off += es[size_t(e)].second.size();
    }
    u.resize(pos, 0);
    u.insert(u.end(), pc.begin(), pc.end());
    f.pach_at.push_back(pos);
    f.ents.push_back(std::move(es));
    pos = (pos + total + 0x7FF) & ~size_t(0x7FF);
    u.resize(pos, 0);
  }
  // the EPAC directory: row order != file order (SOBJ ids 2001.. name PACHs in a shuffled order; BOBJ ids 3001.. the last two + 2005 again -> a different PACH)
  std::memcpy(u.data(), "EPAC", 4);
  std::memcpy(u.data() + 0x800, "SOBJ", 4);
  size_t row = 0x80C;
  auto add = [&](const char* nm, size_t start_byte, size_t size_bytes) {
    std::memcpy(u.data() + row, nm, 4);
    Put32(u, row + 4, uint32_t((start_byte - 0x4000) / 0x800)), Put32(u, row + 8, uint32_t((size_bytes + 255) / 256));
    row += 12;
  };
  auto pach_len = [&](int p) { size_t t = 8 + 12 * f.ents[size_t(p)].size(); for (auto& e : f.ents[size_t(p)]) t += e.second.size(); return t; };
  add("LIST", 0x4000, 0);   // a non-id row: ignored (not a section: "LIST")
  const int perm[10] = {3, 0, 1, 2, 9, 8, 7, 6, 5, 4};
  for (int i = 0; i < 10; ++i) {
    if (g_plant == "drop_row" && i == 4) { f.sobj[2001 + i] = perm[i]; continue; }   // PLANT: PACH 9 is in the file but the directory never names it (a hole in the ranks)
    char nm[8];
    std::snprintf(nm, sizeof nm, "%04d", 2001 + i);
    add(nm, f.pach_at[size_t(perm[i])], pach_len(perm[i]));
    f.sobj[2001 + i] = perm[i];
  }
  std::memcpy(u.data() + row, "BOBJ", 4), row += 12;   // opens the BOBJ section (start 0, size 0)
  add("3001", f.pach_at[10], pach_len(10)), f.bobj[3001] = 10;
  add("3002", f.pach_at[11], pach_len(11)), f.bobj[3002] = 11;
  add("2005", f.pach_at[11], pach_len(11)), f.bobj[2005] = 11;   // also in SOBJ (-> 9... perm[4]) with a different PACH
  // the XCTD wrapper: stored blocks (a zero length word, then the bytes). Block 0 is sized so block 0 ends 10 bytes before a 64 KB file boundary: the writer's zero gap (xctd_file.h, Session 95).
  std::vector<uint32_t> bs;
  size_t nblk = 8;
  for (int it = 0; it < 4; ++it) {
    const size_t table_end = (nblk * 20 + 31) / 32 * 4 + 16;
    const size_t s0 = 0x10000 - 10 - (g_comp ? 20 : 2) - table_end;   // stored: 2-byte zero + s0 bytes; comp: 2 chunk headers + 16 bytes of LZX header + s0 bytes (s0 in 32769..65536 = 2 chunks); both end at 0x10000 - 10
    bs.assign(1, uint32_t(s0));
    size_t left = u.size() - s0;
    while (left) { const size_t s = std::min<size_t>(g_comp ? 0x9013 : 0x6F13, left); bs.push_back(uint32_t(s)); left -= s; }
    if (bs.size() == nblk) break;
    nblk = bs.size();
  }
  const size_t count = bs.size();
  const size_t table_end = size_t((count * 20 + 31) / 32 + 4) * 4;
  Bytes& file = f.file;
  file.assign(table_end, 0);
  file[0] = 0x0F, file[1] = 0xF5, file[2] = 0x12, file[3] = 0xED;
  const uint32_t flags = 1u | uint32_t(count) << 6;   // window 1 << 16, width index 0 = 20 bits
  file[12] = uint8_t(flags >> 24), file[13] = uint8_t(flags >> 16), file[14] = uint8_t(flags >> 8), file[15] = uint8_t(flags);
  for (size_t i = 0; i < count; ++i)
    for (uint32_t b = 0; b < 20; ++b) {
      const size_t bit = i * 20 + b;
      if ((bs[i] >> (19 - b)) & 1) file[16 + bit / 8] |= uint8_t(0x80 >> (bit % 8));
    }
  size_t src = 0;
  for (size_t i = 0; i < count; ++i) {
    const size_t gap = (0x10000 - file.size() % 0x10000) % 0x10000;
    if (gap > 0 && gap < 256) { file.resize(file.size() + gap, 0); f.gap_block = i; }
    if (!g_comp) {
      file.push_back(0), file.push_back(0);
      file.insert(file.end(), u.begin() + std::ptrdiff_t(src), u.begin() + std::ptrdiff_t(src + bs[i]));
    } else {
      // an LZX stream of ONE uncompressed block (type 3, size bs[i]) over 32768-byte frames, one chunk (BE u16 length + bytes) per frame (native_lzx.h): the first chunk = E8 bit 0, type 3, 24-bit size, padded to 32 bits (two LE words, MSB first), R0 R1 R2 = 1 (LE), then the raw bytes; later chunks = raw bytes
      size_t at = 0;
      for (size_t fr = 0; at < bs[i]; ++fr) {
        const size_t n = std::min<size_t>(32768, bs[i] - at);
        Bytes ch;
        if (fr == 0) {
          const uint32_t v = (3u << 28) | (uint32_t(bs[i]) << 4);
          const uint16_t w0 = uint16_t(v >> 16), w1 = uint16_t(v & 0xFFFF);
          ch = {uint8_t(w0), uint8_t(w0 >> 8), uint8_t(w1), uint8_t(w1 >> 8), 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        }
        ch.insert(ch.end(), u.begin() + std::ptrdiff_t(src + at), u.begin() + std::ptrdiff_t(src + at + n));
        file.push_back(uint8_t(ch.size() >> 8)), file.push_back(uint8_t(ch.size()));
        file.insert(file.end(), ch.begin(), ch.end());
        at += n;
      }
    }
    src += bs[i];
  }
  return f;
}

Bytes Slice(const Bytes& b, size_t o, size_t n) { return Bytes(b.begin() + std::ptrdiff_t(o), b.begin() + std::ptrdiff_t(o + n)); }
bool SameEnts(const pac::Pach& a, const Bytes& ab, const pac::Pach& b, const Bytes& bb) {
  if (a.ents.size() != b.ents.size()) return false;
  for (size_t i = 0; i < a.ents.size(); ++i)
    if (a.ents[i].id != b.ents[i].id || a.ents[i].size != b.ents[i].size || std::memcmp(ab.data() + a.ents[i].off, bb.data() + b.ents[i].off, a.ents[i].size) != 0) return false;
  return true;
}
void ResetState() { load_detail::DlcProps() = load_detail::DlcPropState(); }

void WriteFile(const fs::path& p, const Bytes& b) {
  fs::create_directories(p.parent_path());
  std::ofstream o(p, std::ios::binary);
  o.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

int RunUnit() {
  Fixture fx = Build();
  if (g_plant == "corrupt") fx.file[fx.file.size() / 2] ^= 0x5A;   // PLANT: a flipped byte inside a stored block
  if (g_plant == "tail") fx.file.push_back(0);                      // PLANT: one stray byte after the last block (the walk no longer ends at the file size)
  Bytes whole;
  std::string err;
  {
    // the fixture itself is checked against the production whole unpack first (a broken fixture must not pass anything)
    Bytes in = fx.file;
    const bool ok = wwe2k14::xctd::Unpack(in, whole, err);
    Check("fixture_unpack_equals_source", ok && whole == fx.unpacked || g_plant == "corrupt");
  }
  std::vector<uint8_t> blob = fx.file;
  size_t reads_fail_after = 0;
  size_t nreads = 0;
  xr::ReadFn rd = [&](uint64_t off, uint8_t* out, size_t n) {
    ++nreads;
    if (reads_fail_after && nreads > reads_fail_after) return false;
    if (off + n > blob.size()) return false;
    std::memcpy(out, blob.data() + off, n);
    return true;
  };
  // 1. the index
  xr::Index ix;
  const bool built = xr::BuildIndex(rd, blob.size(), ix, err, g_plant != "skip_gap");   // PLANT skip_gap: a READER that ignores the writer's zero gap (honor_gap = false) must not get through the walk
  Check("index_built_walk_ends_at_file_size", built);
  if (built) {
    Check("index_blocks_cover_the_unpacked_bytes", ix.out_total == fx.unpacked.size() && ix.count == ix.sizes.size() && ix.end.back() == blob.size());
    Check("index_gap_block_starts_on_the_64k_boundary", fx.gap_block == 1 && ix.start[1] == 0x10000 && ix.end[0] + 10 == 0x10000);
    Check("index_reads_are_ranged_not_whole", ix.reads < 6 * size_t(ix.count) + 8);
    if (g_comp) {   // the compressed fixture really is compressed: no block starts with the stored marker (00 00), and a block holds 2 chunks (the chunk-header loop of BuildIndex ran more than once)
      bool none_stored = true;
      for (uint32_t i = 0; i < ix.count; ++i) { uint8_t c2[2]; rd(ix.start[i], c2, 2); if (c2[0] == 0 && c2[1] == 0) none_stored = false; }
      Check("comp_fixture_has_no_stored_block", none_stored && ix.end[1] - ix.start[1] == ix.sizes[1] + 16 + 2 * ((ix.sizes[1] + 32767) / 32768));
    }
    // 2. byte-equal decode
    bool eq = true;
    uint32_t seed = 7;
    std::vector<std::pair<uint64_t, uint64_t>> ranges = {{0, 0x4000}, {0, ix.sizes[0]}, {ix.sizes[0] - 5, 10}, {fx.unpacked.size() - 100, 100}, {0, fx.unpacked.size()}, {ix.out_off[2] - 1, ix.sizes[2] + 2}};
    for (int i = 0; i < 40; ++i) { const uint64_t o = Rng(seed) % (fx.unpacked.size() - 1), n = 1 + Rng(seed) % std::min<uint64_t>(90000, fx.unpacked.size() - o); ranges.push_back({o, n}); }
    for (auto [o, n] : ranges) {
      Bytes out;
      if (!xr::DecodeRange(rd, ix, o, n, out, err) || out != Slice(fx.unpacked, size_t(o), size_t(n))) eq = false;
    }
    Check("decode_range_equals_the_whole_unpack", eq);
    Bytes out;
    Check("decode_range_past_the_end_refused", !xr::DecodeRange(rd, ix, fx.unpacked.size() - 3, 10, out, err));
    // a read that fails mid-decode is an error, never a short / zero result
    nreads = 0, reads_fail_after = 1;
    Check("decode_read_failure_is_an_error", !xr::DecodeRange(rd, ix, 0, fx.unpacked.size(), out, err));
    reads_fail_after = 0;
  }
  // refusals: a stored file (count 0), a non-XCTD file, a truncated file
  {
    Bytes bad = fx.file;
    bad[0] = 'E';
    blob = bad;
    xr::Index x2;
    Check("index_refuses_a_non_xctd_file", !xr::BuildIndex(rd, blob.size(), x2, err));
    blob = Bytes(fx.file.begin(), fx.file.end() - 7);
    Check("index_refuses_a_truncated_file", !xr::BuildIndex(rd, blob.size(), x2, err));
    blob = fx.file;
  }
  // 3. the production path through GameFiles: ranged ordinal + slices equal the whole open
  const fs::path root = fs::path("out") / "SKB3" / (g_comp ? "fixture_root_comp" : "fixture_root");
  fs::remove_all(root);
  WriteFile(root / "pac" / "evt" / "evtobj.pac", fx.file);
  launcher::GameFiles files;
  files.SetRoot(fs::absolute(root));
  ResetState();
  pac::Pac whole_pac;
  const bool wo = pac::Open(files, "pac\\evt\\evtobj.pac", whole_pac, err);
  const bool tail_fixture = g_plant == "tail";
  if (g_plant != "corrupt") Check("fixture_whole_open_parses_every_pach", wo && whole_pac.pachs.size() == 12);
  ResetState();
  const auto t0 = std::chrono::steady_clock::now();
  load_detail::EvtRanged* r = load_detail::OpenEvtRanged(files, err);
  Check("ranged_open_succeeds", r != nullptr);
  std::printf("      ranged open %.2f ms (%s)\n", Ms(t0), r ? "ok" : err.c_str());
  if (r) {
    int wrong = 0, other_ok = 0;
    for (const auto& [id, want] : fx.sobj) {
      int other = -1;
      if (g_plant == "drop_row" && id == 2005) continue;
      if (PropPachEvt(files, uint32_t(id), &other) != want) ++wrong;
      if (id == 2005 && other == 11) other_ok = 1;
    }
    for (const auto& [id, want] : fx.bobj)
      if (id != 2005 && PropPachEvt(files, uint32_t(id), nullptr) != want) ++wrong;
    Check("pach_rank_equals_the_files_pach_order", wrong == 0);
    Check("pach_in_both_sections_reports_the_bobj_one", other_ok == 1 || g_plant == "drop_row");
    // slices
    bool sl_eq = true;
    for (int p = 0; p < 12; ++p) {
      if (p == 9 && g_plant == "drop_row") continue;
      const pac::Pac* s = load_detail::OpenEvtSlice(files, p, err);
      if (!s || s->pachs.size() != 1 || p >= int(whole_pac.pachs.size()) || !SameEnts(s->pachs[0], s->bytes, whole_pac.pachs[size_t(p)], whole_pac.bytes)) sl_eq = false;
    }
    Check("slice_entries_equal_the_whole_opens_pach", sl_eq);
    Check("whole_pac_is_never_held", load_detail::DlcProps().cache.find("pac\\evt\\evtobj.pac") == load_detail::DlcProps().cache.end());
    // PropPachEvt's directory ordinal agrees with the whole pac's own directory walk (PropDirIn) for every id
    int dis = 0;
    for (const auto& [id, want] : fx.sobj) { (void)want; if (PropPachEvt(files, uint32_t(id)) != PropDirIn(&whole_pac, uint32_t(id), false, false, true)) ++dis; }
    Check("ranged_ordinal_equals_PropDirIn_of_the_whole_pac", dis == 0);
    // a failing ranged read: the slice is refused (the caller takes the whole-file path), nothing is half-cached
    load_detail::DlcProps().evt.slices.clear();
    auto good = load_detail::DlcProps().evt.rr.read;
    load_detail::DlcProps().evt.rr.read = [](uint64_t, uint8_t*, size_t) { return false; };
    Check("slice_with_a_failing_read_is_refused", load_detail::OpenEvtSlice(files, 3, err) == nullptr && load_detail::DlcProps().evt.slices.empty());
    Check("failed_slice_is_remembered_not_decoded_again", load_detail::DlcProps().evt.slice_failed.count(3) == 1 && [&] { load_detail::DlcProps().evt.rr.read = good; const int d = load_detail::DlcProps().evt.slice_decodes; const bool refused = load_detail::OpenEvtSlice(files, 3, err) == nullptr; return refused && load_detail::DlcProps().evt.slice_decodes == d; }());   // S105 SKB3 fix L1
    load_detail::DlcProps().evt.slice_failed.clear();
    load_detail::DlcProps().evt.rr.read = good;
    Check("slice_works_again_when_reads_do", load_detail::OpenEvtSlice(files, 3, err) != nullptr);
    // BellRelease's ReleaseEvtobj: frees the ranged state, only for a releasable slot; a re-ask re-opens
    Check("release_refused_when_not_releasable", !load_detail::ReleaseEvtobj());
    load_detail::DlcProps().releasable = true;
    Check("release_frees_the_ranged_state", load_detail::ReleaseEvtobj() && !load_detail::DlcProps().evt.ok && load_detail::DlcProps().evt.slices.empty());
    Check("ranged_reopens_after_release", load_detail::OpenEvtSlice(files, 5, err) != nullptr);
  } else if (tail_fixture) {
    // FALLBACK: the ranged walk refused (stray byte), the whole-file path must still answer the same ordinal
    ResetState();
    Check("fallback_ordinal_via_the_whole_file", PropPachEvt(files, 2001) == fx.sobj[2001] && load_detail::DlcProps().cache.count("pac\\evt\\evtobj.pac") == 1);
  }
  return g_fail ? 1 : 0;
}

int RunFallbackUnit() {   // the real "ranged refused -> whole" path with a fixture the walk refuses (a stray byte), as its own check set
  g_plant = "tail";
  Fixture fx = Build();
  fx.file.push_back(0);
  const fs::path root = fs::path("out") / "SKB3" / "fixture_tail";
  fs::remove_all(root);
  WriteFile(root / "pac" / "evt" / "evtobj.pac", fx.file);
  launcher::GameFiles files;
  files.SetRoot(fs::absolute(root));
  ResetState();
  std::string err;
  Check("fallback_ranged_open_refused_on_a_stray_tail_byte", load_detail::OpenEvtRanged(files, err) == nullptr);
  Check("fallback_ordinal_via_the_whole_file", PropPachEvt(files, 2001) == fx.sobj[2001] && load_detail::DlcProps().cache.count("pac\\evt\\evtobj.pac") == 1);
  g_plant.clear();
  return 0;
}

int RunReal(const std::string& game, bool verify, bool all) {
  launcher::GameFiles files;
  files.SetRoot(fs::path(game));
  std::string err;
  ResetState();
  auto t = std::chrono::steady_clock::now();
  load_detail::EvtRanged* r = load_detail::OpenEvtRanged(files, err);
  if (!r) { std::printf("FAIL ranged open: %s\n", err.c_str()); return 1; }
  std::printf("index + directory: %.1f ms (%u blocks, %zu reads, directory %zu PACHs, unpacked %llu bytes)\n", Ms(t), r->ix.count, r->ix.reads, r->rows.size(), (unsigned long long)r->ix.out_total);
  pac::Pac whole;
  if (verify) {
    t = std::chrono::steady_clock::now();
    if (!pac::Open(files, "pac\\evt\\evtobj.pac", whole, err)) { std::printf("FAIL whole open: %s\n", err.c_str()); return 1; }
    std::printf("whole open (the old path): %.1f ms, %zu PACHs\n", Ms(t), whole.pachs.size());
    Check("real_directory_rank_covers_every_pach", whole.pachs.size() == r->rows.size());
  }
  const int ids[] = {3571, 2038, 2004, 3004, 2718, 2720, 3038};   // (2591, Fandango: a DLC prop with no disc directory row: PropPach opens the whole pac, the old path, by design)
  for (int id : ids) {
    t = std::chrono::steady_clock::now();
    const int ord = PropPachEvt(files, uint32_t(id));
    const double ms_ord = Ms(t);
    if (ord < 0) { std::printf("prop %d: no directory row (a node-rule / DLC prop: PropPach, whole-pac path)\n", id); continue; }
    t = std::chrono::steady_clock::now();
    const pac::Pac* s = load_detail::OpenEvtSlice(files, ord, err);
    const double ms_slice = Ms(t);
    if (!s) { std::printf("FAIL prop %d PACH %d slice: %s\n", id, ord, err.c_str()); ++g_fail; continue; }
    t = std::chrono::steady_clock::now();
    Model m;
    std::string e2;
    const bool ok = LoadPropEntry(files, ord, 0, m, e2);
    const double ms_load = Ms(t);
    std::printf("prop %d -> PACH %d: ordinal %.2f ms, slice %.2f ms (%zu bytes), LoadPropEntry(entry 0) %.2f ms %s%s\n", id, ord, ms_ord, ms_slice, s->bytes.size(), ms_load, ok ? "ok" : "failed: ", ok ? "" : e2.c_str());
    if (verify) {
      const bool eq = size_t(ord) < whole.pachs.size() && SameEnts(s->pachs[0], s->bytes, whole.pachs[size_t(ord)], whole.bytes);
      Check((std::string("real_slice_equals_whole_pach_") + std::to_string(ord)).c_str(), eq);
    }
  }
  std::printf("slices decoded: %d; the whole 181 MB pac held: %s\n", r->slice_decodes, load_detail::DlcProps().cache.count("pac\\evt\\evtobj.pac") ? "YES" : "no");
  if (all) {
    // S105 SKB3 fix M1 (folded in from the reviewer's all-props check): the real file is LZX-compressed in every block; the gate's fixture alone never decoded one
    if (!pac::Open(files, "pac\\evt\\evtobj.pac", whole, err)) { std::printf("FAIL whole open: %s\n", err.c_str()); return 1; }
    int stored = 0, comp = 0;
    for (uint32_t i = 0; i < r->ix.count; ++i) { uint8_t c[2]; if (!r->rr.read(r->ix.start[i], c, 2)) { ++g_fail; continue; } (c[0] == 0 && c[1] == 0 ? stored : comp)++; }
    std::printf("blocks %u (stored %d, compressed %d), rows %zu, pachs %zu, unpacked %llu\n", r->ix.count, stored, comp, r->rows.size(), whole.pachs.size(), (unsigned long long)r->ix.out_total);
    Check("all_blocks_are_compressed_as_on_the_players_disc", comp > 0);
    std::vector<uint8_t> every;
    const bool dec = xr::DecodeRange(r->rr.read, r->ix, 0, r->ix.out_total, every, err);
    Check("all_full_decode_range_equals_the_whole_unpack", dec && every == whole.bytes);
    every.clear(), every.shrink_to_fit();
    Check("all_rows_cover_every_pach", r->rows.size() == whole.pachs.size());
    int base_bad = 0, short_bad = 0, slice_bad = 0;
    for (size_t i = 0; i < std::min(r->rows.size(), whole.pachs.size()); ++i) {
      if (r->rows[i].first != whole.pachs[i].base) ++base_bad;
      if (r->rows[i].second < whole.pachs[i].end - whole.pachs[i].base) ++short_bad;
      const pac::Pac* sl = load_detail::OpenEvtSlice(files, int(i), err);
      if (!sl || sl->pachs.size() != 1 || !SameEnts(sl->pachs[0], sl->bytes, whole.pachs[i], whole.bytes)) ++slice_bad;
    }
    Check("all_rows_first_equals_pach_base", base_bad == 0);
    Check("all_rows_not_shorter_than_the_pach_extent", short_bad == 0);
    Check("all_slices_equal_the_whole_opens_pach", slice_bad == 0);
    std::set<uint32_t> ids;
    for (size_t o = 0x80C; o + 12 <= 0x4000; o += 12) {
      const uint8_t* q = r->header.data() + o;
      if (std::isdigit(q[0]) && std::isdigit(q[1]) && std::isdigit(q[2]) && std::isdigit(q[3])) ids.insert(uint32_t(std::atoi(std::string(reinterpret_cast<const char*>(q), 4).c_str())));
      else if (q[0] == 0 && Le32(r->header, o + 4) == 0) break;
    }
    int dis = 0, other_dis = 0;
    for (uint32_t id : ids) {
      int other = -1;
      const int rg = PropPachEvt(files, id, &other);
      const int sj = PropDirIn(&whole, id, false, false, true), bj = PropDirIn(&whole, id, true, false);
      const int want = sj >= 0 ? sj : bj, want_other = (sj >= 0 && bj >= 0 && sj != bj) ? bj : -1;
      if (want >= 0 && rg != want) ++dis;
      if (want >= 0 && other != want_other) ++other_dis;
    }
    std::printf("directory ids %zu, ordinal disagreements %d, other disagreements %d\n", ids.size(), dis, other_dis);
    Check("all_directory_ids_exist", ids.size() > 100);
    Check("all_ordinals_equal_PropDirIn_of_the_whole_pac", dis == 0);
    Check("all_other_ordinals_equal", other_dis == 0);
  }
  Check("real_whole_pac_never_held", load_detail::DlcProps().cache.count("pac\\evt\\evtobj.pac") == 0);
  return g_fail ? 1 : 0;
}
}  // namespace

int main(int argc, char** argv) {
  std::string real;
  bool verify = false, all = false;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--plant") && i + 1 < argc) g_plant = argv[++i];
    else if (!std::strcmp(argv[i], "--real") && i + 1 < argc) real = argv[++i];
    else if (!std::strcmp(argv[i], "--verify")) verify = true;
    else if (!std::strcmp(argv[i], "--all")) all = true;
    else if (!std::strcmp(argv[i], "--comp")) g_comp = true;
  }
  if (!real.empty()) return RunReal(real, verify, all);
  const int rc = RunUnit();
  if (g_plant.empty() && !g_comp) RunFallbackUnit();
  return g_fail ? 1 : rc;
}
