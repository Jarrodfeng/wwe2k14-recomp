"""qmenu_lodge: what a route lane needs about menus that the game's flow table does NOT hold (S105 QM). Our own annotations only
(ids, row order, press counts, state words, sources). Row text is stored as the LABEL TEXT the player sees (a short label, like the atlas's);
`q menu build` resolves every label to its text id(s) in the game's own string tables, and `qmenu_selftest.py` fails when a label stops
resolving. Marks: [C] confirmed (run / listing / string table, cited), [L] reasoning, [U] unknown (and what would settle it).

Where each fact came from:
  R3  = docs/research/s105/R3_caw_save.md (route lane, ~16 game runs, sheets of runs 073-112)
  N2  = docs/research/s101/N2_EDITOR_DRIVER.md (the editor read from drawn text)
  T5  = docs/research/s105/T5_menu_scenes.md (settle probe, code + runs 057-070)
  atlas = docs/systems/game_atlas.json + tools/atlas/screens.json (list layer read from the flow table, crawl runs 5889-5896)
"""

# The Create a Superstar editor (sequence 76, ctor sub_833979B8, "Create a Superstar Ex Menu Sequence").
# Page = (title, [(row label, what A does / note)]).  Text ids come from the string tables (bank 0500 leaf 3, ids 6340-6751).
CAW = {
    'seq': 76,
    'enter': "list 3 item 0 CREATE A SUPERSTAR (screen 1502, exit 8) > list 14 NEW / EDIT > template; B at the editor top leaves to the main menu [C R3]",
    'tabs': [  # RB = next tab, LB = previous; the red dot of four at x 362 + 16 k is the tab [C N2]
        ('HEAD', 'tab 0 [L: start tab; R3 "HEAD tab"; RB x2 from the start reaches CLOTHING [C R3]]'),
        ('BODY', 'tab 1 [L: the only tab left between HEAD and CLOTHING; never opened in a route: U]'),
        ('CLOTHING', 'tab 2 [C N2: dot 2]'),
        ('OTHER', 'tab 3 [C N2: dot 3]'),
    ],
    'pages': {
        'HEAD': ['FACES', 'TEMPLATES', 'SKIN', 'HAIR', 'FACE', 'FACIAL HAIR', 'MAKEUP', 'FACEPAINT', 'BODY ART'],       # [C R3]
        'CLOTHING': ['TEMPLATES', 'HEADWEAR', 'UPPER BODY', 'LOWER BODY'],                                               # [C R3, N2]
        'UPPER BODY': ['OUTFITS', 'TOPS', 'JACKETS', 'BODY ACCESSORIES', 'ARMWEAR', 'EDIT LAYERS'],                     # [C R3]
        'OTHER': ['EDIT RING ATTIRE', 'EDIT ENTRANCE ATTIRE', 'EDIT CINEMATIC ATTIRE', '...', 'ACCEPT'],               # [C N2: 9 rows; rows 4-8 not named: U]
    },
    # row -> what opens (R3 editor map)
    'opens': {
        'HEAD/HAIR': 'SUPERSTAR > a carousel of hair styles > A > options: COLOR ETC / LENGTH / ACCEPT (DOWN x2, A) [C R3]',
        'CLOTHING/UPPER BODY': 'page UPPER BODY [C R3]',
        'UPPER BODY/TOPS': 'a carousel of tops > A (variant) > A (options: COLOR ETC / LOGOS / PATTERN / ACCEPT) [C R3]',
        'TOPS options/LOGOS': 'LOGOS page: DOWN x4 = CUSTOM LOGO DATA > one stick-down = the first saved logo > A > notice > LEFT = YES > A (placement) > A [C R3]',
        'UPPER BODY/BODY ACCESSORIES': 'a carousel; the slot is single (re-entering shows DELETE / REPLACE) [C R3]',
    },
    # option counts: shown on screen as "N / M"; M is from the player's save / parts table, NOT yet read from the game's data
    'counts': {
        'HEAD/HAIR carousel': (86, '[C R3: "57/86"; count not derived from the parts table: U]'),
        'UPPER BODY/BODY ACCESSORIES carousel': (38, '[C R3: "1/38"; not derived: U]'),
    },
    'waits': [
        'wait_state seq 76 600 5                 # the editor is the sequence (0x839400F8>0>48 == 76) [C]',
        'wait_frames 40..60 between presses      # NO memory word follows the page or the row [C S101 N0-1: seq+64 / +96 / +34924 did not move]; R3 used 40-60 frame waits, 20 drops presses [C R3]',
        '!caw state (live verb, owner mode)      # page title, rows, hot row and tab dot read from the drawn text (src/caw_live.h; tools/n2/caw_driver.py) [C N2]',
    ],
    'open': 'a page / cursor word in guest memory: the page object at seq+34924 (vtable slot 9 called each tick) and the nested state seq+96 are the leads; none read yet; the page code sub_833A0628 keeps an index at +1172 and a count at +1176 of its object (read and written, decomp --fields) and finds records with sub_834247B8 (the flow-table record format, docs/formats/menu_pages.md section 2) [U which is the row: a watch on the hot row '
            'text colour writer, docs/systems/caw_driver.md]',
}

# Screens whose state words the atlas does not give (T5, settle probe, code + runs 057-070).
SCREENS = {
    'finisher': {
        'node': 'mode:finisher',
        'title': 'Create a Finisher (3D preview with both wrestlers)', 'seq': 26,
        'enter': 'CREATIONS item 4 SPECIAL MOVE > list 15 NEW (item 0) > list 28 FRONT GRAPPLE (item 0, screen 1511, exit 14) [C T5, atlas]',
        'up': 'wait_state 0x839400F8>0>72 10 1800 2   # editor page up, 3D flags on [C code sub_82271768/sub_82271530; value never logged live]',
        'back': 'empty timeline: one B -> seq 75 in 33 frames, then B x3 to menu 1 [C run 057]',
    },
    'title': {
        'node': 'screen:1506',
        'title': 'Create a Title', 'seq': 75,
        'enter': 'CREATIONS item 7 CHAMPIONSHIP (screen 1506, mode 18: a 2D list); sequence 29 (CAB MENU) only after A on a row and A in its popup [C T5]',
        'up': 'wait_state menu 1506 ; wait_state screen_mode 18 300 2 optional',
        'back': 'B',
    },
    'streak': {
        'node': 'mode:wm30',
        'title': 'The Streak (CStreakMeunSequence)', 'seq': 36,
        'enter': '30 YEARS (main item 1, screen 1500, exit 2 -> seq 31) > RIGHT x5 to the 6th card (after Hulkamania + 4 LOCKED eras) > A (controller result 4) [C T5]',
        'up': 'wait_state seq 36 [C: sequence_base_ctor(36, "CStreakMeunSequence") sub_82827258]',
        'back': 'B from the era screen -> menu 1 in one press (141 frames) [C T5 E18]',
    },
    'wm30': {
        'node': 'mode:wm30',
        'title': '30 Years of WrestleMania era cards', 'seq': 31,
        'enter': 'main item 1 (screen 1500, exit 2) [C atlas]; RIGHT / LEFT move one card, DOWN does nothing, A on an era -> seq 35 CWM30MeunSequence (result 3) [C T5]',
        'up': 'wait_state seq 31', 'back': 'B x4 to list 1 [C atlas run 5892]',
    },
    'universe': {
        'node': 'mode:universe',
        'title': 'WWE Universe', 'seq': 75, 'enter': 'main item 2 (screen 1497, mode 15) [C atlas]',
        'up': 'wait_state menu 1497 ; wait_state screen_mode 15', 'back': 'B x3 to list 1 [C atlas]',
    },
    'replay': {
        'node': 'screen:1515',
        'title': 'MY WWE > REPLAY (Create a Highlight)', 'seq': 27,
        'enter': 'MY WWE item 4 (screen 1515, exit 21) -> seq 27 CCahSequenceBase; greyed on the frozen save [C T5]',
        'up': 'wait_state seq 27', 'back': 'B',
    },
}

# The named state words of tools/input_scripts/states.toml that every menu route uses (read from the file by `q menu build`, listed here only as the idiom).
IDIOM = [
    'include boot_to_menu ; wait_state menu 1 1200 30',
    'press_until_state DOWN cursor >=K 12 30 5 if_needed ; press_until_state UP cursor K 12 30 5 if_needed   # put the cursor on item K',
    'press_until_state A menu N 8 120 5            # open list / screen N (menu = 0x8392945C>364)',
    'press_until_state A seq S 8 120 5             # open a mode (a new sequence S = 0x839400F8>0>48)',
    'press_until_state B menu M 12 90 5            # back out',
]
