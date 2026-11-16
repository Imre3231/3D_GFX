# Music player — PIC32 side

Tracker modules (MOD, S3M, XM, IT) and recordings (MP3, FLAC, WAV) play on
the FPGA (MOD_TOP). For a module the PIC32 does what `gfx_cow.py` does for a
3D scene: it turns the file into one fixed SDRAM layout (the *image*) and
streams it to the loader in chunks; then the FPGA plays it. A recording goes
out as it is, as it plays, and the FPGA's CPU decodes it (§3).

Everything that is only *encoding* in a module is undone on the PIC — packed
patterns, parapointers, delta / ADPCM / IT214 / IT215 samples, 8/16 bit,
signed/unsigned, byte order, interleaved stereo — so the FPGA reads the same
layout for every format. Playback *semantics* stay native: notes, effects and
volume-column bytes are the format's own, and where trackers disagree on how a
file should be read, `MUSIC.h` follows OpenMPT.

| File | |
|---|---|
| `MUSIC.h` | modules: parser, converter and chunk writer |
| `AUDIO.h` | new — recordings: tags, the audio data, streaming (§3) |
| `newmain.c` | the player (playlist, shuffle, keys, end of song); browser keys |
| `FS_GENERIC.h` | folders of up to 10 000 entries, names with accents, NTFS read anew (§9) |
| `GUI.h` | `SHUFFLE PLAY` row, icons, message box |
| `ICON.h` | icons: modules (notes on the page), recordings (level bars), shuffle |

---

## 1. Using it

A file is a module if its extension is `.MOD` `.S3M` `.XM` `.IT` (any case)
or it has the Amiga prefix `MOD.name`, a recording if it is `.MP3`, `.FLAC` /
`.FLA` or `.WAV` / `.WAVE`. A file whose name says neither is recognized by
its first bytes: a module signature (`IMPM`, `Extended Module: `, `SCRM` at
44, `M.K.` `M!K!` `FLT4` `FLT8` `xCHN` `xxCH` `xxCN` at 1080), `fLaC`,
`RIFF`…`WAVE`, or an ID3v2 tag (MP3, or FLAC behind it). Each format has its
own icon.

The open folder is the playlist, modules and recordings together:

| Key | While playing |
|---|---|
| — | song ends: the next song; after the last one, the first |
| DOWN | next song |
| UP | previous song |
| Enter | back to the browser, cursor on the last song played |

The keys also work while a module loads (the load stops at the next chunk
boundary) and while a recording streams (the PIC stops at the next chunk). A
song picked in the browser that cannot be played is refused there, with the
reason (`CANNOT PLAY` / `NOT AN XM FILE`, `RATE ABOVE 96 KHZ`, …); one the
playlist comes to is skipped. If the FPGA stops taking data, the player
returns to the browser with `LOAD FAILED` / `FPGA NOT RESPONDING`.

**Shuffle.** A folder with two songs or more has a `SHUFFLE PLAY` row (after
`[ BACK ]`; the footer shows how many songs). It plays the songs in random
order: every song once a lap, a new order each lap; UP goes back along it.

**Large folders.** A folder may hold 10 000 entries (before: 900). UP or DOWN
held repeats: after 400 ms a row every 130 ms, after 2 s every 60 ms, after
5 s 1 % of the folder every 0.2 s, after 10 s 5 %. The footer shows
`row/rows`.

Songs only play with the music bitstream in the FPGA. With another one loaded
(video, 3D) the data goes into that design's input: expect `LOAD FAILED` for a
module within about a second; a recording streams into nothing and Enter
leaves it.

---

## 2. Link

Pins are unchanged: SPI1 data, `MODE_VGA` (RG15) and `DREQ` (RE5).
`MODE_VGA` goes high when the player starts and stays high, across all songs,
until Enter.

### Chunks

The `g3d_loader.v` format, big-endian:

```
A5 3D  type[2]  addr[4]  len[4]  sum[4]  payload[len x 4]
```

`addr` is an SDRAM word address, `len` counts 32-bit words, `sum` is the 32-bit
sum of the payload words (0 for an empty chunk). A chunk always goes out whole.

| Type | Payload |
|---|---|
| `MH` | module header, 64 words |
| `CH` | channel table, 64 words |
| `OR` | order list |
| `PT` | pattern table |
| `ZP` | zero block (empty patterns point here) |
| `MM` | IT MIDI macros |
| `PD` | pattern data |
| `IN` | instrument records |
| `SM` | sample records |
| `SD` | sample data |
| `AH` | audio header, 64 words (§3) |
| `AD` | audio data, at most 256 words, into the ring (§3) |
| `GO` | none; `addr` = header address: the image is complete (module) or the stream starts (recording) |
| `CT` | none; control, command in `addr[7:0]`: `1` = STOP. Other values reserved |

A module goes out as `MH CH OR PT [ZP] [MM] PD… (IN | SM SD…)… GO`; payloads
are at most 4096 words. A recording goes out as `AH GO AD AD …`. Before the
next song, and when the player is left, the PIC sends `CT STOP`. A load or a
stream a key abandons ends after some chunk; the next song starts with `MH` or
`AH`.

### DREQ

Flow control, as for video. While a module loads, DREQ may be low for at most
`MUS_DREQ_TIMEOUT` polls (about half a second) in all while one chunk goes
out: a top that takes the data slower fails the load. A recording goes out as
it plays: the FPGA holds DREQ low while its ring is full, for as long as the
song takes to make room — a fraction of a second, more at a very low bit
rate. The PIC waits up to `AUD_DREQ_MS` (10 s) for it, reading the keys
meanwhile.

**End of song: the FPGA holds DREQ low for 50 ms** (25–200 ms works). Once all
of a song has gone out (a module's `GO`, a recording's last chunk) the PIC
treats DREQ low for 20 ms as the end of the song, sends `CT STOP` and starts
the next one. Outside that pulse DREQ must then never stay low for more than a
few microseconds: for a recording the FPGA lets its last chunks in without a
hold (§3).

### FPGA side

MOD_TOP (FPGA project, `README_FPGA.md`) is the other end: its loader writes
the payloads to SDRAM, `GO` starts the player on the header (`MODP` or
`AUDP`), `CT STOP` and a new `MH` or `AH` silence it, and DREQ is
`~fifo_almost_full & ~end_pulse & ~hold`.

---

## 3. Audio streams

`AUDIO.h`. `fw/host/audio_stream.py` (FPGA project) builds the same stream on
a PC.

### Data

| Format | Sent |
|---|---|
| MP3 | from the first byte after an ID3v2 tag to the end, without an ID3v1 tag and an APE tag there |
| FLAC | `fLaC`, STREAMINFO marked as the last metadata block, the frames: pictures, tags, seek table and padding stay behind |
| WAV | the whole file |

The format is the content's: `fLaC` (after an ID3v2 tag, if there is one) is
FLAC, `RIFF`…`WAVE` is WAV, anything else MP3. A `.flac` or `.wav` file that
is not that inside is refused.

The bytes go 4 to a word, the first in bits 31:24, the last word zero-padded.
Word k of the stream is at ring + (k mod ring words); an AD chunk carries 256
words (the last one fewer), so none runs past the ring's end.

### Header (64 words, at `0x100000`)

| Word | |
|---|---|
| 0 | `0x41554450` `'AUDP'` |
| 1 | 31:24 version, `1`; 23:16 format: 1 MP3, 2 FLAC, 3 WAV |
| 2 | stream bytes |
| 3 | ring address, `0x200000` |
| 4 | ring words, `0x40000` (1 MB) |
| 5 | file bytes |
| 21 | 31:16 / 15:0 track: the song's place in the folder / songs in the folder |
| 24–39 | file name, 64 bytes |
| 40–47 | title, 32 bytes |
| 48–55 | artist, 32 bytes |
| 56–63 | album, 32 bytes |

Other words are 0; strings are packed as in a module's header (§5).

| Format | Title, artist, album |
|---|---|
| MP3 | the first `TIT2` `TPE1` `TALB` of an ID3v2.3 / 2.4 tag (`TT2` `TP1` `TAL` in 2.2), Latin-1, UTF-16 or UTF-8; without any of them, ID3v1 |
| FLAC | `TITLE=` `ARTIST=` `ALBUM=` of the Vorbis comments; without any of them, an ID3v2 tag before `fLaC` |
| WAV | `INAM` `IART` `IPRD` of a `LIST INFO` chunk |

Text shows as the file names do (§9): Latin-1 and Latin Extended-A letters
without their accents, other characters `_`, tabs and line ends as spaces; up
to 32 characters, leading and trailing spaces dropped.

### Flow and the end

The FPGA's CPU reads the ring as it plays and sets a limit: DREQ is low while
the AD words received reach the words it has read plus the ring's size less
2048 (room for chunks on their way). It lifts the limit once the rest of the
stream is within a chunk of fitting, so that DREQ stays high after the last
chunk; the end-of-song pulse comes when the last sample has been played. A
stream it cannot decode (`CANNOT PLAY THIS FILE` on its screen) it takes in
and drops, then sends the end pulse: the playlist goes on.

### What the PIC refuses

The FPGA's limits (`fw/target/audio.c`), checked before anything goes out, so
that a long file is not sent only to be dropped:

| | Refused | Message |
|---|---|---|
| FLAC | no 34-byte STREAMINFO | `NOT A FLAC FILE` |
| | more than 2 channels | `MORE THAN 2 CHANNELS` |
| | above 96 kHz | `RATE ABOVE 96 KHZ` |
| | blocks over 8192 samples, fewer than 4 bits | `FLAC NOT SUPPORTED` |
| | frames over 40 956 bytes (STREAMINFO's maximum) | `FLAC FRAMES TOO LARGE` |
| WAV | not PCM (float, compressed) | `NOT PCM AUDIO` |
| | other than 1–8 channels, 4–192 kHz, 8/16/24/32 bits, block align channels × bytes | `WAV NOT SUPPORTED` |
| `.flac`, `.wav` | not that inside | `NOT A FLAC FILE`, `NOT A WAV FILE` |

An MP3 is not checked: the FPGA finds its frames.

---

## 4. SDRAM

All addresses are word addresses, all words 32-bit. `0x000000` up is the
frame buffer(s), free for the FPGA.

A module:

| Address | |
|---|---|
| `0x100000` | image: header (`MUS_BASE`) |
| `+64` | channel table, 64 words |
| `+128` | order list, up to 256 words |
| `+384` | pattern table, up to 256 × 2 words |
| `+896` | MIDI macros, instruments, samples, zero block, pattern data, sample data — in that order, addresses in the header |
| `< 0x800000` | end of image (header word 19) |

The PIC refuses a module whose image would pass `0x800000` (32 MB).

A recording:

| Address | |
|---|---|
| `0x100000` | header, 64 words (`AUD_BASE`) |
| `0x200000` | the ring, `0x40000` words (`AUD_RING`) |

---

## 5. Module header (64 words)

| Word | Bits | |
|---|---|---|
| 0 | | `0x4D4F4450` `'MODP'` |
| 1 | 31:24 | version, `1` |
| | 23:16 | format: 1 MOD, 2 S3M, 3 XM, 4 IT |
| | 15:8 | effect family: 0 MOD/XM, 1 S3M/IT |
| | 7:0 | channels (1–64) |
| 2 | 31:16 / 15:0 | orders / pattern table entries |
| 3 | 31:16 / 15:0 | instruments (0 = sample mode) / samples |
| 4 | 31:16 | initial tempo (BPM) |
| | 15:8 | initial speed (ticks per row) |
| | 7:0 | restart order (MOD, XM) |
| 5 | 31:24 | global volume: 0–64 (MOD, S3M, XM), 0–128 (IT) |
| | 23:16 | mix volume (S3M, IT) |
| | 15:8 / 7:0 | IT separation / pitch-wheel depth |
| 6 | | flags, below |
| 7 | | tracker version: S3M `ffi << 16 \| cwtv`, XM version, IT `cmwt << 16 \| cwt` |
| 8 | | raw: MOD magic, S3M `flags \| mv << 16 \| uc << 24`, XM flags, IT `flags \| special << 16` |
| 9 | | raw: MOD restart byte, S3M `dp \| gv << 8 \| gus << 16`, IT row highlight |
| 10–19 | | addresses: channel table, order list, pattern table, instruments (0 if none), samples, MIDI macros (0 if none), zero block (0 if none), pattern data, sample data, end |
| 20 | | note display offset, signed: MOD −36, S3M −12, XM −12, IT 0 |
| 21 | 31:16 / 15:0 | track: this module's place in the folder / modules in the folder |
| 24–39 | | file name, 64 bytes |
| 48–55 | | song name, 32 bytes |
| 56–63 | | tracker name, 32 bytes |

Other words are 0. Strings are packed 4 bytes per word, first byte in bits
31:24, NUL-padded; control bytes are replaced by spaces, trailing spaces
dropped. A note `n` shows as name `(n−1+offset) mod 12`, octave
`(n−1+offset) / 12` — C-2 for 61 in a MOD, C-4 in S3M and XM, C-5 in IT.

Flags (word 6):

| Bit | |
|---|---|
| `0x0001` | linear frequency slides (XM, IT) |
| `0x0002` | instrument mode (IT) |
| `0x0004` | stereo (S3M, IT) |
| `0x0008` | IT old effects |
| `0x0010` | IT compatible Gxx |
| `0x0020` | S3M fast volume slides |
| `0x0040` | S3M Amiga limits |
| `0x0080` | S3M ST2 vibrato |
| `0x0100` | MOD `M.K.` / `M!K!`: ProTracker rules, Amiga note range |
| `0x0200` | MOD VBlank timing (NoiseTracker, Startrekker): `Fxx` only sets speed |
| `0x0400` | XM saved by FastTracker 2 |
| `0x0800` | S3M has Adlib instruments (silent here) |
| `0x1000` | IT MIDI macros in the image |
| `0x2000` | IT channels beyond 64 were dropped |
| `0x4000` | MOD 15-sample Soundtracker file |

### Channel table (64 words)

| Bits | |
|---|---|
| 7:0 | initial pan, 0 left – 255 right, `0x80` centre |
| 15:8 | channel volume, 0–64 |
| 16 | muted |
| 17 | surround |
| 18 | unused channel |
| 31:24 | raw: S3M channel setting, IT pan byte |

### Order list and pattern table

One word per order: a pattern number. S3M and IT keep their markers — `0xFE`
skip, `0xFF` end of song; in a MOD a value of `0x80` or more is not a pattern.

Pattern table entry, two words: the pattern's data address, then
`rows | flags << 16` — `0x1` empty, `0x2` missing from the file (with `0x1`).
Every pattern the order list names has an entry; an empty or missing one has
its row count (64 when missing) and points at the zero block.

---

## 6. Pattern cells

A pattern is `rows × channels` cells, row after row, 2 words per cell:

```
word 0:  note[31:24]  instrument[23:16]  volume column[15:8]  has_volume[0]
word 1:  effect[31:24]  parameter[23:16]  (15:0 zero)
```

Notes: 0 none, 1–120 = C-0…B-9 with **61 = middle C**, `0xFD` fade,
`0xFE` cut, `0xFF` note off.

| | MOD | S3M | XM | IT |
|---|---|---|---|---|
| note | 25–108, nearest period | 13–120 | 13–108 | 1–120 |
| special | — | `FE` (`^^`) | `FF` (key off) | `FD` `FE` `FF` |
| instrument | sample 1–31 | sample 1–99 | instrument | instrument, or sample in sample mode |
| volume column | — | 0–64, 128–192 pan | XM byte `10`–`50`, `60`–`FF` | IT byte 0–124, 128–212 (223–232 OpenMPT) |
| effect | `0`–`F` | 1–26 = A–Z | 0–35 = `0`–`9`, `A`–`Z` | 1–31 (A–Z, OpenMPT extensions) |

`has_volume` tells volume 0 from no volume. Effect 0 with parameter 0 is no
effect (MOD, XM: an arpeggio needs a non-zero parameter). Effect bytes the
format does not define are stored as 0 with parameter 0. A MOD period is
stored as the nearest note of the ProTracker table (OpenMPT's rule), clamped to
25–108.

---

## 7. Instruments and samples

### Instrument record (256 words, XM and IT)

| Word | |
|---|---|
| 0 | `nna \| dct << 8 \| dca << 16 \| flags << 24` — flags: `1` pan used, `2` cutoff used, `4` resonance used, `8` old IT instrument |
| 1 | fadeout per tick, 65536 = full volume |
| 2 | `gvol (0–128) \| pan << 8 \| pps << 16 (signed) \| ppc << 24 (note)` |
| 3 | `random vol \| random pan << 8 \| cutoff << 16 \| resonance << 24` |
| 4 | auto-vibrato `type \| sweep << 8 \| depth << 16 \| rate << 24` (XM) |
| 5 | `MIDI channel \| program << 8 \| bank << 16` |
| 6 | samples in the instrument |
| 8–34 | volume envelope |
| 35–61 | panning envelope |
| 62–88 | pitch / filter envelope |
| 96–215 | key map, notes 1–120: `sample (1-based, 0 none) \| note << 16` |
| 216–223 | name |

Envelope (27 words): `points | loop start << 8 | loop end << 16 | flags << 24`,
then `sustain start | sustain end << 8`, then 25 nodes `tick | value << 16`
(value signed: volume 0–64, panning and pitch −32…+32). Envelope flags: `1` on,
`2` loop, `4` sustain, `8` carry, `0x10` filter (pitch envelope).

### Sample record (32 words)

| Word | |
|---|---|
| 0 | data address (left channel of a stereo sample) |
| 1 | length in frames |
| 2, 3 | loop start, loop end (end exclusive) |
| 4, 5 | sustain loop start, end (IT) |
| 6 | flags, below |
| 7 | C5 speed: Hz at middle C (S3M, IT; 8363 for MOD and XM) |
| 8 | `volume (0–64) \| global volume << 8 (0–64) \| pan << 16` |
| 9 | `finetune \| relative note << 8`, both signed: MOD −8…+7 (eighths), XM −128…+127 (1/128 semitone) |
| 10 | auto-vibrato `type \| sweep << 8 \| depth << 16 \| rate << 24` (IT) |
| 11 | right channel address (stereo), else 0 |
| 16–23 | name |
| 24–26 | DOS file name (S3M, IT) |

| Flag | |
|---|---|
| `0x001` | has data |
| `0x002` | loop |
| `0x004` | ping-pong loop |
| `0x008` | sustain loop |
| `0x010` | ping-pong sustain loop |
| `0x020` | 16-bit in the file |
| `0x040` | stereo |
| `0x080` | default pan used |
| `0x100` | Adlib instrument — no data |
| `0x200` | compressed / ADPCM in the file |
| `0x400` | data missing or not decodable (Ogg) — plays silent |

### Sample data

Signed 16-bit mono frames, two per word: frame 2k in bits 15:0, frame 2k+1 in
bits 31:16. 8-bit data is scaled by 256. A stereo sample is two such channels
of the same length. Each channel starts on a 4-word boundary and is followed by
at least 8 zero words, for interpolation past the end.

### MIDI macros (IT)

The file's MIDI configuration as it is: 4896 bytes (9 global, 16 `SFx`,
128 `Zxx` macros of 32 bytes), packed like the strings. Present when header
flag `0x1000` is set.

---

## 8. Conversion rules

Where libxmp and OpenMPT read a file differently, `MUSIC.h` does what OpenMPT
does:

- MOD: channels from the magic (`M.K.` `M!K!` `M&K!` `FEST` `N.T.` `PATT`
  `NSMS` `LARD` `FLT4/8` `EXO4/8` `xCHN` `xxCH` `xxCN` `CD61` `CD81` `OKTA`
  `OCTA` `TDZx` `FA0x`); without one, OpenMPT's 15-sample Soundtracker
  checks, else refused. Loop rules of `MODSampleHeader::ConvertToMPT`;
  Soundtracker loop start in bytes, data before it skipped. Startrekker: FLT8
  pattern pairs, `Exx` dropped, `Fxx` capped at `1F`. ModPlug ADPCM samples
  decoded.
- S3M: channels up to the last one enabled (4 if none); default pans `0x33`
  / `0xCC`, or the file's pan table; tempo below 33 reads as 125; a sample of
  type 0 has no data.
- XM: orders past the header size read as 0; envelopes clamped; a missing
  instrument stays empty; Ogg samples silent.
- IT: channels counted from the patterns, at most 64; IT214 / IT215
  decompression as OpenMPT's `ITDecompression`; invalid effect parameters
  cleared.
- XM and IT: envelope ticks saved with only their low byte are repaired.
- Every format: a sample that runs past the end of the file is cut to the
  frames present; loops are clamped to the length.

### Limits

| | |
|---|---|
| channels | 64 (IT extras dropped, flag `0x2000`) |
| orders / patterns | 256 / 256 |
| rows per pattern | 1024 |
| instruments / samples | 255 / 512 (more is refused) |
| image | 7 M words above `0x100000` |

Ultimate Soundtracker effect numbers (`1xy` arpeggio, `2xy` pitch bend) are
not converted: a 15-sample file (flag `0x4000`) keeps its raw effects.

---

## 9. Folders and the file system

`FS_GENERIC.h` lists a folder into 8-byte entries: the type, and where the
entry's directory record is (FAT32, exFAT: its cluster and record; NTFS: its
MFT record). 10 000 entries take 80 KB; the 900 of before took 86 KB. Name,
size and first cluster are read back from the directory when asked for, and
the last 16 are kept, so a page of the browser costs a read or two.

- **Names.** Long names (FAT32, exFAT, NTFS) keep their letters: Latin-1 and
  Latin Extended-A accents are dropped (`Ő` → `O`), other characters show as
  `_` (before: every character past ASCII). On FAT32 a long name whose
  checksum does not match its 8.3 entry (one a deleted file left) is not
  used, and 8.3 names stored as lowercase (Windows, Linux) show lowercase.
- **Types.** From the name. Files whose names do not say are looked into when
  the folder opens if there are 64 or fewer, else when they first show.
- **Reading.** FAT32 and exFAT folders are read 8 sectors at a time. An exFAT
  folder laid down in one piece (no FAT chain) ends at its length.
- **NTFS.** A folder's index is walked as the B-tree it is, from the root,
  each entry after those below it: the names come sorted, as Explorer shows
  them, and only blocks the tree uses are read. Index blocks are found by
  their VCN for any cluster size: before, only the first index block of each
  cluster was read, so with clusters over 4 KB most files were missing (162
  of 3000 with 64 KB clusters), and blocks no longer in use were listed.
  Attributes in extension records (`$ATTRIBUTE_LIST`) are followed: the
  index of a folder of thousands of files (0 of 10 000 were listed before), a
  file in hundreds of pieces (its runs read in an extent at a time), a name
  in an extension record. The MFT's own runs locate records when it is
  fragmented. A file's size is its `$DATA`'s.

---

## 10. Fixes in existing code

The module reader is the first code to read more than one sector at a time
with multi-sector reads off, and to jump backwards inside a file. That
exposed two bugs in `FS_GENERIC.h`, both now fixed:

- **`FS_Read_CurrentFile_Bytes` skipped data.** With `g_fs_multi_read_ok` at
  0 it read one sector but advanced past every contiguous sector it had
  found, leaving the rest of the buffer stale.
- **The cluster map had no slot 0.** `chain_index[0]` was never set, so a
  backward jump into the second of the file's 64 map strides walked the chain
  from cluster 0 and the read failed — on FAT32 for any file, on exFAT for
  fragmented ones. Video seeks go through the same code.

Plain files are now sniffed once (`VFS_TYPE_OTHER`) instead of on every
repaint.

`GUI_Draw_Footer()` copied `PARENT FOLDER` (14 bytes with its NUL) into a
12-byte buffer on the stack; the buffer is now 16 bytes.

---

## 11. Verification

`MUSIC.h` was compiled natively (`gcc -m32 -std=gnu89`) against a model of the
board and the loader, and checked against libxmp, an independent parser:

- **524 modules** (libxmp's and OpenMPT's test suites): 492 read exactly as
  libxmp reads them — 586,955 pattern cells and 42.7 M sample frames
  compared. 9 differ, all fuzz tests, Startrekker AM synth files, or places
  where OpenMPT and libxmp disagree; 23 are refused (Ice Tracker,
  ChipTracker, truncated).
- **Whole firmware**, browsing FAT32 (512-byte clusters, fragmented files),
  exFAT and NTFS images: the playlist path — end-of-song pulses, DOWN, UP, a load
  abandoned by a key, a broken file skipped both ways, wrap-around — produced
  10 loads, each byte-identical to the converter's reference stream, with
  `CT STOP` between them. Also checked: the error box, a DREQ stall
  (`FPGA NOT RESPONDING`), an FPGA that trickles the data in (load fails in
  0.4 s; short key presses during slow loads are honoured), Enter during a
  load, random DREQ drops.

The music player and the file system, the same way:

- **Folders.** `FS_GENERIC.h` on images of 10 000 files with long, accented
  and emoji names, 12 extensions and sizes of 0–3000 bytes: FAT32 (4 KB and
  32 KB clusters), exFAT (32 KB and 128 KB clusters, a folder in one piece),
  NTFS (4 KB clusters, the folder's index in extension records; 64 KB
  clusters). Every name, type and size as written, NTFS in its sorted order.
  Opening the folder of 10 000 took 307 reads of up to 4 KB on FAT32, 379 on
  exFAT, 785 on NTFS. Fragmented files (FAT32 with 512-byte clusters, exFAT,
  NTFS with a 6 MB file in 4 extents of about 770 runs) read back right, every
  sector.
- **Recordings.** 8 files as one playlist with a module, for several laps:
  MP3 with ID3v2.3 and ID3v1, with APE and ID3v1, with a 50 KB picture and
  UTF-16 tags; FLAC with Vorbis comments and a picture, behind an ID3v2.4
  tag, without an extension; WAV with `LIST INFO`. Every stream byte-identical
  to `audio_stream.py`'s. A 9.3 MB MP3 and an 8.6 MB WAV through the FPGA's
  ring at 400 kB/s and at 24 kB/s: identical, DREQ held up to 255 ms; DOWN, UP
  and Enter stopped the stream 15–170 ms after the press. A float WAV and a
  192 kHz FLAC are refused with their messages.
- **Shuffle.** 114 songs in 14 laps: each lap all 8 songs once, each lap in
  another order; DOWN ×3, UP ×4 went back the way it came.
- **Fast skip** in the folder of 10 000: 45 rows in the first 5 s, then 1 %
  steps, then 5 % steps; the page complete when the key is let go.
- **Modules** as before: the playlist, the broken file, a cancelled load and
  the DREQ stall, every load byte-identical.

No new warnings under `-Wall -Wextra`. RAM: 362 KB of static data, 12 KB more
than the module player's: the 10 000 browser entries take less than the 900
did, the NTFS walker and its run maps take more. The reader and chunk buffers
borrow the video stream buffers, idle while music plays.
