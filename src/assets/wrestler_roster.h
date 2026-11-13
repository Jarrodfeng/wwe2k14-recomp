// The wrestler roster: every character model of the player's own pac\ch\chNNN.pac files, read once, so a stream draw's model can be found BY CONTENT
// (Session 101, lane 45 = W2; docs\renderer\WRESTLER_PROMOTION_MAP.md sections 3 and 10; the method is lane 42's tools\wrestler\wrestler_pair.cpp).
// Copyright (c) the wwe2k14-recomp authors. BSD-3-Clause.
//
// An Entry is one JBOY body of one attire of one character pac (entry ids 0, 26, 27 and 10000-39999, the pieces the stream draws as models); it keeps the COUNTS
// the join needs (per mesh record: vertices, index count, bone palette) and no vertex data. A Model is one entry read in full (the decoded blob + its
// assets::jboy_parts view) for the parts we draw. Until the declared link (R1: the reader hook, WRESTLER_PROMOTION_MAP.md section 3) exists, content is the join:
// a stream object fits an entry when every drawn part has a mesh record with the same vertex and index counts. DLC pacs (chNNN_dlcPP.pac) are NOT searched.
//
// Nothing here is game data at rest: it reads the player's files at run time. No SDK includes. The .cpp holds its own static stb_image (zlib for pac.h), like
// src\assets\crowd_body.cpp.
#pragma once
#include <atomic>
#include <functional>
#include <future>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// pac.h needs stb_image.h's declaration (stbi_zlib_decode_buffer) before it. This header only DECLARES it (the implementation is static inside wrestler_roster.cpp),
// so a translation unit that includes this header must not call pac::Decode / pac::Open itself.
#ifndef STB_IMAGE_IMPLEMENTATION
#if __has_include("stb_image.h")
#include "stb_image.h"
#else
#include "../../thirdparty/rexglue-sdk-src/thirdparty/stb/stb_image.h"  // (main checkout layout; the build gives the folder with -I)
#endif
#endif
#include "jboy_parts.h"
#include "pac.h"

namespace assets::wrestler_roster {

struct PartInfo {
  uint32_t vertices = 0, index_count = 0, palette_count = 0;
  uint32_t palette[20] = {};
  bool ok = false;
};
struct Entry {
  int pac = 0, attire = 0;
  uint32_t entry = 0;
  uint32_t node_count = 0;
  std::vector<PartInfo> parts;
};

// One entry in full. `parts.parts[i].s[]` / `groups[].data` point into `blob`: the Model is never copied or moved after Load.
struct Model {
  Bytes blob;
  jboy_parts::Parts parts;
  int entry_index = -1;
};

class Roster {
 public:
  Roster();
  ~Roster();
  Roster(const Roster&) = delete;
  Roster& operator=(const Roster&) = delete;
  // Reads every pac\ch\chNNN.pac of `game_root` (counts only). Blocking; call it from a worker. False + error when nothing was read.
  bool Build(const std::string& game_root, std::string& error);
  // The same on a worker thread (Poll() tells when it is done).
  void BuildAsync(const std::string& game_root);
  // lane 153 (N3, the Create a Superstar editor's wrestler): the editor assembles its body at run time from per-region part files (pac\edit\edNN.pac, PACH leaves such as 2001-2058, each
  // 1-3 meshes). This indexes every mesh of every DISTINCT leaf by counts (a leaf that repeats byte for byte across the pacs' archives is kept once). A separate Roster instance
  // does this (the wrestlers' own roster is untouched, and pays nothing for it). Poll() / error() / seconds() are the same state word.
  static constexpr int kCawBackdropPacId = 1066;   // lane 169 (N3b): EditPart::pac 1066 = pac\bg\bg66.pac, the editor's backdrop (CAS_wall, CAS_moya), not an edNN.pac
  struct EditPart {
    uint16_t pac = 0, pach = 0;     // pac\edit\ed<pac>.pac, its top-level PACH archive number
    uint32_t leaf = 0, mesh = 0;    // the entry id in that archive and the mesh record = the game's part index inside the leaf
    uint32_t vertices = 0, index_count = 0, palette_count = 0;
    uint32_t group = 0;             // index of the distinct leaf (all meshes of one leaf share it)
  };
  // cache_path (may be empty): the counts index (ids and counts only, never game bytes) is read from / written to this file on the PLAYER's machine, keyed by the sizes and times of
  // the edNN.pac files; a hit makes BuildEdit take milliseconds instead of ~20 s of inflating 61 pacs (review S3).
  bool BuildEdit(const std::string& game_root, std::string& error, const std::string& cache_path = std::string());
  void BuildEditAsync(const std::string& game_root, const std::string& cache_path = std::string());
  void DropEditCache();   // the one edNN.pac LoadEditLeaf keeps open (tens of MB inflated) is released
  const std::vector<EditPart>& edit_parts() const { return edit_parts_; }
  std::shared_ptr<Model> LoadEditLeaf(size_t edit_part_index, std::string& error);   // the leaf the part is in, parsed (null + error on failure)
  // 0 not started / building, 1 ready, 2 failed (error()).
  int Poll() const { return state_.load(); }
  const std::string& error() const { return error_; }
  const std::vector<Entry>& entries() const { return entries_; }
  size_t pacs() const { return pacs_; }
  double seconds() const { return seconds_; }
  // Reads entry `index` in full (a worker or the caller's thread). Null + error on failure.
  // lane 142: `keep_pac` leaves the unpacked pac open for the next load (the entries of one character's pac are read back to back: 13 attire candidates of one hat re-opened
  // and re-unpacked the same tens of MB 13 times); TrimIdle() lets it go once no load asked for it for `seconds`. Default false: the old behaviour.
  std::shared_ptr<Model> LoadModel(int index, std::string& error, bool keep_pac);
  std::shared_ptr<Model> LoadModel(int index, std::string& error) { return LoadModel(index, error, false); }
  void TrimIdle(double seconds);   // lane 142: never blocks (a load in progress keeps the pac)
  // lane 142: ONE loader thread reads the queued entries in FIFO-by-pac order (the job of the pac that is already open first, else the lowest (pac, attire, entry)), so a frame that
  // asks for 13 models is one thread and one pac unpack, not 13 threads contending on the mutex. The future is ready when the model is read (null on failure).
  std::shared_future<std::shared_ptr<Model>> LoadModelAsync(int index, bool keep_pac);
  int PacOpens() const;                                        // lane 142 (test): how many times a pac was opened by a load
  void SetArrivalOrderForTest(bool on);                        // lane 142 (test plant): the loader takes jobs in arrival order, ignoring the open pac
  int OpenPacId() const;                                       // lane 142 (test): the pac a keep_pac load left open, -1 none
  void LockedForTest(const std::function<void()>& f);          // lane 142 (test): runs f while the load mutex is held
  void Join();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  std::vector<Entry> entries_;
  std::vector<EditPart> edit_parts_;   // lane 153
  std::string error_, root_;
  size_t pacs_ = 0;
  double seconds_ = 0;
  std::atomic<int> state_{0};
};

}  // namespace assets::wrestler_roster
