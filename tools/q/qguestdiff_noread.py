r"""The parameter names the created-superstar (CAW) records carry that no Material field takes, each with the reason no field needs to
(S111 UNREADNAMES, docs\research\s111\UNREADNAMES.md). `q guestdiff` prints a CAW name neither path reads into a field; a name one of
these rows covers is printed with the row's reason instead, so a NEW unread name stands out on the line of bare names.

A row = (pattern, mark, reason): pattern = an exact name or an fnmatch glob; mark = C (settled from the code / the files), L (likely),
U (open: the reason says what is open). Every reason is the game's side: no name-table row (sub_82BAAEE8: no such string in the
image), a register no yCh_* pixel shader reads (the 272 yCh_* pixel shaders of _startup.pac / edit.pac disassembled), or a value
equal in every record of the player's files (stock, edit and mods pacs; the asset index) to what ours already feeds there.
A row that covers no unread name any more (a path reads it now, or no record carries it) is printed as stale: drop it.
"""
import fnmatch

NO_STRING = 'no string of this name in the game image (q str), so the name table sub_82BAAEE8 has no row for it: no register is written from it'

NOREAD = (
    ('g_f4GlowMskCol[1-4]', 'C', NO_STRING),
    ('g_fEnvBumpRate', 'C', NO_STRING),
    ('g_fEnvLightRate', 'C', NO_STRING),
    ('g_fRimAdjSpcMas', 'C', NO_STRING),
    ('g_texDiffuse', 'C', "no string of this name in the game image (yDefault's diffuse row is texDiffuse, string 0x82069CFC): the name table never matches it"),
    ('g_f4FaceLv_Wud', 'C', 'PS c201.y (row 0x8392A440): no yCh_* pixel shader reads c201.y or c201.z (c201 only as .w, the diffuse LOD bias); 0.0 in every record'),
    ('g_f4FaceLv_Bld', 'C', 'PS c201.z (row 0x8392A460): no yCh_* pixel shader reads c201.y or c201.z; 0.0 in every record'),

    ('g_fSpecVtxALev', 'C', 'PS c143.x (name copied at 0x82BAC5D0): no yCh_* pixel shader declares or reads c143'),
    ('g_fSpecVtxAPow', 'C', 'PS c143.y (name copied at 0x82BAC608) and the 4th channel of the spec-power cube bake (sub_825AB5E0): no yCh_* pixel shader reads c143 or the w of a sampler-11 fetch'),
    ('g_fIdMapLv[0-7]', 'C', "VS c237 / c238 (rows from 0x8392ACA0): the draw takes the model's live override row (sub_82BD6780; ours CharCtx::idmap, wrk_join.h); 0.0 in every record = the zero ours feeds without a row"),
    ('g_fSSS_[ls]', 'L', 'no name-table row: only the pre-light slot setup sub_827EB348 reads them (the light-cube bake, slot object +252 / +256); 0.5 / 0.1 in every record'),
    ('g_mCubeLPrm', 'U', 'pixel sampler 13 (row 0x8392AC60), the light-cube slot: 119 of 120 edit records name cube_light, which no pac holds (nothing is bound; the light cube stays, as in ours); open: the one ed20 yCh_TransTiling record naming CUBEMAP'),
)


def reason_of(name, table=NOREAD):
    """-> the first row whose pattern is the name or matches it as a glob, else None."""
    for row in table:
        if name == row[0] or fnmatch.fnmatchcase(name, row[0]):
            return row
    return None


def split(names, table=NOREAD):
    """-> (bare, why, stale): bare = the names no row covers (sorted); why = [(pattern, mark, reason, [names])] in table order for the
    rows that cover some of the names; stale = the patterns that cover none of them."""
    hit, bare = {}, []
    for n in sorted(names):
        row = reason_of(n, table)
        if row is None:
            bare.append(n)
        else:
            hit.setdefault(row[0], []).append(n)
    why = [(p, m, r, hit[p]) for p, m, r in table if p in hit]
    stale = [p for p, _m, _r in table if p not in hit]
    return bare, why, stale
