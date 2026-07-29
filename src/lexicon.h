/**
 * lexicon.h — offline dictionary from SD (M11, PocketMage-inspired).
 *
 * Data lives at /sdcard/lexicon/dict.txt (sorted "key\tdefinition" lines,
 * multiple lines per word = senses) + dict.idx (two-letter-prefix byte
 * offsets). Build both with tools/build_lexicon.py (WordNet 3.1) and copy
 * them over via the Files app's USB transfer. Type a word, Enter looks it
 * up, Up/Down cycle senses.
 */
#pragma once

// Open the Lexicon app. Registers its launcher leave hook.
void lexicon_open();
