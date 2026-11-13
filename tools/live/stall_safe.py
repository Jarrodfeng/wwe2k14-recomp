"""S101 lane 145: the one file that says which on-screen messages the stall watchdog (stall_guard.py) may answer by itself.

Anything not matched here is NEVER pressed: it could be a save, delete or overwrite prompt. The watchdog stops and says what
it saw instead. Adding an entry means answering "why is one A (or START) press on this text safe?" in its `why`.

Text comes from the game's text manager (`!status` text:, src/screen_text.h): the strings the screen fetched, joined by " | ".
A message body drawn from glyph codes arrives as "???" (the J3 box read "??? OK").
"""
import re

# A segment that names the answer: (name, regex matched against the WHOLE segment, case-insensitive, button, why it is safe).
BUTTONS = [
    ('ok', r'(?:press\s+)?(?:a\s*[:=-]?\s*)?ok', 'A',
     'a one-button box: OK offers no other choice, and A is what the player presses. A box with a second choice shows a second '
     'button word (Yes / No / Cancel), which VETO stops.'),
    ('press_a', r'press\s+a(?:\s+to\s+continue)?', 'A', '"Press A" asks for exactly this press and offers no other choice.'),
    ('a_alone', r'a', 'A', 'a lone "A" button label is the confirm of a one-button notice.'),
    ('continue', r'continue(?:\s*[.>]*)?', 'A',
     'a continue prompt (load screens, 30 Years cards, results) moves forward; nothing is saved or deleted by it.'),
    ('press_start', r'press\s+start(?:\s+button)?', 'START', 'the title screen start prompt: opens the main menu, changes nothing.'),
]

# A body segment that may sit beside a button segment: (name, regex for the WHOLE segment, why, only_in_a_match).
# The glyph-only body is allowed only while a match is running (seq == 9): save / storage prompts do not appear mid-match, the
# J3 box did.
CARDS = [
    ('glyph_body', r'[?\s.!*]*\?[?\s.!*]*', 'a message body drawn from glyph codes (no readable word); a match box only.', True),
    # [U] the exact text of the 30 Years objective card is index-table text ("Win the match in under 6 minutes"): matched by
    # shape and unconfirmed. It is only a card: text, no choice on it.
    ('objective_card', r'(?:historical\s+)?objectives?\b.*|(?:win|finish|complete|perform|hit|land|keep)\b.*',
     '[U] the 30 Years objective card: text only, no choice on it.', True),
]

# Words that veto the press whatever else is on screen: a second choice, or something that writes or removes data.
VETO = re.compile(r'\b(save|saving|overwrite|delete|erase|remove|format|storage|device|memory\s+unit|quit|exit|restart|abandon|'
                  r'purchase|buy|unlock|sign\s*in|sign\s*out|xbox\s+live|yes|no|cancel|back|decline|accept|confirm|replace|'
                  r'discard|reset|default|are\s+you\s+sure|corrupt|error)\b', re.I)

# states.toml seq value for "in a match".
SEQ_IN_GAME = 9
