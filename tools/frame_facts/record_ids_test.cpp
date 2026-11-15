// S102 R21-A: ParseArenaId (src\renderer_scene\record_ids.h) against the old classification (sscanf "arena:slot:%d%7s" + the prefix test) over a corpus of ids and a generated sweep.
// No game, no SDK, no GPU. Exit 0 = every id gives the same (kept, n, k, twin) as the old code.
// "--plant1" (the short prefix) / "--plant2" (the :r tail ignored): the planted fault MUST make the table differ: exit 0 when the test sees it, 1 when it does not.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../src/renderer_scene/record_ids.h"

struct Old { bool kept; int n, k; bool twin; };
static Old OldOf(const std::string& id) {
  Old o{false, 0, 0, false};
  int k = 0;
  char tail[8] = {};
  const int n = std::sscanf(id.c_str(), "arena:slot:%d%7s", &k, tail);
  if (n < 1 || id.rfind("arena:slot:", 0) != 0) return o;
  o.kept = true, o.n = n, o.k = k, o.twin = n == 2 && tail[0] == ':' && tail[1] == 'r';
  return o;
}
static Old NewOf(const std::string& id, bool plant1, bool plant2) {
  Old o{false, 0, 0, false};
  const rdr_scene::ArenaId a = rdr_scene::ParseArenaId(id, plant1);
  if (a.n < 1) return o;
  o.kept = true, o.n = a.n, o.k = a.k, o.twin = a.n == 2 && a.tail[0] == ':' && a.tail[1] == 'r' && !plant2;
  return o;
}
int main(int argc, char** argv) {
  const bool plant1 = argc > 1 && !std::strcmp(argv[1], "--plant1"), plant2 = argc > 1 && !std::strcmp(argv[1], "--plant2");
  std::vector<std::string> ids = {"", "arena", "arena:slot:", "arena:slot:0", "arena:slot:7", "arena:slot:123", "arena:slot:12:r", "arena:slot:12:r1", "arena:slot:12:x", "arena:slot:12:", "arena:slot:-5",
                                  "arena:slot:+5", "arena:slot: 5", "arena:slot:5 ", "arena:slot:5 :r", "arena:slot:5abc", "arena:slot:abc", "arena:slot::r", "arena:slo:5", "arena:slox:12", "Arena:slot:1",
                                  "xarena:slot:1", "arena:slot:1234567890", "arena:slot:5:rabcdefghij", "arena:slot:5\t:r2", "crowd:member:5", "arena:slot:0x10", "arena:slot:9:r:r", "arena:slot:4294967297", "arena:slot:2147483648", "arena:slot:99999999999", "arena:slot:-2147483649", "arena:slot:4294967295:r"};
  const char* tails[] = {"", ":r", ":r1", ":x", " ", "abc", ":", ":rr", " :r", "\t"};
  const char* pre[] = {"arena:slot:", "arena:slox:", "arena:slo", "arena:", "x"};
  const char* num[] = {"", "0", "7", "42", "-1", "+3", " 9", "007", "12345", "a"};
  for (const char* p : pre)
    for (const char* n : num)
      for (const char* t : tails) ids.push_back(std::string(p) + n + t);
  int bad = 0;
  for (const std::string& id : ids) {
    const Old a = OldOf(id), b = NewOf(id, plant1, plant2);
    if (a.kept != b.kept || (a.kept && (a.n != b.n || a.k != b.k || a.twin != b.twin))) {
      ++bad;
      if (!plant1 && !plant2) std::printf("MISMATCH id=\"%s\" old(kept %d n %d k %d twin %d) new(kept %d n %d k %d twin %d)\n", id.c_str(), a.kept, a.n, a.k, a.twin, b.kept, b.n, b.k, b.twin);
    }
  }
  std::printf("[record_ids_test] %zu ids, %d differ%s\n", ids.size(), bad, plant1 ? " (plant1: the short prefix)" : plant2 ? " (plant2: twins dropped)" : "");
  if (plant1 || plant2) return bad > 0 ? 0 : 1;
  return bad == 0 ? 0 : 1;
}
