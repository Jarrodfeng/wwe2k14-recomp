// S110 SEAT3-R: a small library for the game's step 3 run (included by test_core.cpp, flag `--guests N [--residents R]`).
//
//   importer_test.exe --game DIR --dlc DIR --mods OUT_DIR --source PAC --guests 3 [--residents 3]
//
// makes <OUT_DIR>\guests\superstars: R + N characters built by the importer's own BuildPack (all seated: BuildPack gives every card a free seat while one
// is free), then the LAST N cards are made library characters: their card.txt says `seat none`. (S110 LIBSIZE-2) BuildPack makes EVERY card in the one form (seat_card.h
// kRefSeat 497 / kRefBank 9997, `file_form 2`: library\<KEY>\pac\ch\ch497.pac + rows.bin for the reference identity, the same for a seated card and a guest), so
// flipping the seat to `seat none` is all a guest needs; his files are not touched. The game takes such a card (seats_core.h FormRefusal) and the test reads the
// roster back with the game's own reader (LoadRoster) to prove it: R residents seated (re-targeted to their seats at load), N guests, nothing left out. Free frame
// seats remain, so the game's pages have seats to bind them in. The importer's own check calls these cards made (a seat is no part of a card's files), so a
// launcher PLAY / BUILD on this folder keeps them as they are.
#pragma once

static void GuestLibrary(const Env& env, int guests, int residents) {
  std::printf("a library of %d resident(s) and %d guest(s) for the game's step 3 run:\n", residents, guests);
  std::string pac;
  const std::string mods = FreshMods(env, "guests", &pac);
  const std::vector<int> seats = importer::FreeSlots(env.gf, env.dlc);
  if (!Say(!pac.empty() && int(seats.size()) >= residents + guests && guests > 0, "the source is extracted and the frame has seats for " + std::to_string(residents + guests) + " characters (" + std::to_string(seats.size()) + ")"))
    return;
  importer::PackState st;
  for (int i = 0; i < residents + guests; ++i)
    st.stars.push_back(MakeStar(pac, seats[size_t(i)], i < residents ? "Resident " + std::to_string(i + 1) : "Guest " + std::to_string(i - residents + 1)));
  std::string e;
  if (!Say(importer::SavePack(mods, st, e) && Build(env, mods, st, env.dlc, ""), "BuildPack makes the " + std::to_string(residents + guests) + " characters (all seated)")) return;
  importer::PackState back;
  importer::LoadPack(mods, back, e);
  int flipped = 0;
  for (const importer::Superstar& s : back.stars) {
    if (s.name.rfind("Guest ", 0) != 0) continue;
    const fs::path cp = Root(mods) / "library" / s.key / "card.txt";
    sc::Card c;
    std::string ce;
    if (sc::ReadCard(Slash(cp), c, ce) && c.inst.valid) {
      c.seat = -1;
      flipped += sc::WriteCard(Slash(cp), c, &ce);
    }
    std::printf("    guest %s: key %s, made for the reference identity %d / %d (file_form %d)\n", s.name.c_str(), s.key.c_str(), c.inst.seat, c.inst.bank, sc::FileForm(c));
  }
  const Bytes ft = Read(Root(mods) / "frame" / "superstars.txt");
  seatsn::Roster ro;
  seatsn::LoadRoster(Root(mods), PackIdOf(mods), std::string(ft.begin(), ft.end()), ro);
  Say(flipped == guests && int(ro.res.size()) == residents && int(ro.lib.size()) == guests && ro.left_out.empty() && ro.unseated == guests,
      "the game's own reader (LoadRoster) takes it: " + std::to_string(ro.res.size()) + " resident(s) seated, " + std::to_string(ro.lib.size()) + " library character(s), " +
          std::to_string(ro.left_out.size()) + " left out");
  for (const std::string& l : ro.left_out) std::printf("    left out: %s\n", l.c_str());
  std::printf("  the mods folder to run the game on: %s\n", mods.c_str());
}
