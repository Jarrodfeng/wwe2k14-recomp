// S101 lane 131: the movie planes' factory for the replace seam (replace_factories.h). Included by replace_factories.h after its types; do not include directly.
// --replace-fault NAME[,NAME] (fview / rplay): stale_planes, bt601_vs_709, no_alpha, swapped_uv.
#pragma once
#include "movie_ours.h"

namespace rdr_scene {

inline ReplaceHookHandle MakeMovieOurs(const ReplaceFactoryArgs& a) {
  ReplaceHookHandle h;
  if (!a.renderer) {
    h.error = "no renderer";
    return h;
  }
  MovieFaults faults;
  std::string err;
  if (!MovieFaults::Parse(a.fault, faults, err)) {
    h.error = err;
    return h;
  }
  auto hook = std::make_unique<MovieOurs>(*a.renderer, faults);
  MovieOurs* raw = hook.get();
  h.report = [raw]() { return raw->Report(); };
  h.hook = std::move(hook);
  return h;
}

}  // namespace rdr_scene
