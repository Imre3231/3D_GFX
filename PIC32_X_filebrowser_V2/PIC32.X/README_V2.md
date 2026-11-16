# PIC32 media player — v2.0

Boot screen, fast file browser, and MKV fast-forward / rewind with an on-screen
progress bar.

Six new headers carry the work; `newmain.c` is rewritten around them and
`USB_MSC_BARE.h` has four small edits. Everything else is untouched.

---

## 1. Files

| File | State | What it does |
|---|---|---|
| `GFX.h` | new | Drawing primitives, text, pipelined SPI writes |
| `BOOT.h` | new | Boot console: label/value slots, detail and error lines |
| `GUI.h` | new | Dark-theme browser, sidebar scrollbar, partial redraw |
| `STREAM.h` | new | FPGA transport, lifted out of `newmain.c` |
| `MKV_SEEK.h` | new | EBML parsing, timeline, cluster scan, seek engine |
| `OSD.h` | new | Red progress bar shown while seeking |
| `newmain.c` | rewritten | Boot sequence, browser loop, players |
| `USB_MSC_BARE.h` | patched | Debug output routed into the boot console |
| `MUSIC.h` | new | Tracker modules → FPGA player image (`README_MUSIC.md`) |
| `AUDIO.h` | new | MP3, FLAC and WAV streamed to the FPGA player (`README_MUSIC.md`) |

The new headers are `#include`d by `newmain.c`, so MPLAB will build without
touching the project file. Adding them to the project's header list only
affects the IDE's file tree.

Include order in `newmain.c` matters — `GFX.h` and `BOOT.h` must come before
`USB_MSC_BARE.h`, because the USB code now calls `Boot_USB_Line()`.

---

## 2. Boot screen

`Boot_Begin()` paints a title bar and ten label/value slots; `Boot_Set()`,
`Boot_Value()` and `Boot_Set_U32()` fill them in. Two lines at the bottom carry
running commentary (`Boot_Detail`) and failures (`Boot_Error`).

What it reports: CPU clock, SPI1 clock to the FPGA (computed from `PB2DIV` and
`SPI1BRG`, not hard-coded), frame buffer geometry, USB host state, drive
vendor/product, capacity, filesystem type, cluster size, root directory, and
the mount result.

The USB enumeration trace used to call `text3()` at `line * USBH_DBG_DY`,
scattering messages down the screen at fixed positions and overwriting whatever
was there. `USBH_dbg_line()` now forwards to `Boot_USB_Line()`, which keeps only
the newest message on the detail line. `Boot_Active()` gates every draw, so once
`Boot_End()` runs, nothing in the USB layer can paint over the browser or over
video.

`USB_WaitReconnect()` reopens the console rather than clearing the screen, so a
drive pulled out mid-session shows the same interface.

---

## 3. Browser

Dark theme from the mockup: `#22262E` background, red accent, separator rules.
Header carries the path, footer carries `selected/total` and the file size.
Six rows of 33 px, 30×30 icon at x=8, text at x=46.

The scrollbar occupies x=303..316 — a track, triangle arrow caps top and
bottom, and a rounded thumb whose length and position follow the list.

**The speed comes from not repainting.** `GUI_Render()` keeps shadow copies of
the scroll position, selection and item count:

| Change | Repaint | Pixel writes |
|---|---|---|
| Move selection | 2 rows + footer | ~35 k |
| Scroll one row | 6 rows + thumb | ~60 k |
| Change directory | Everything | ~77 k |

The old code called `fillScreen()` plus six rows on every keypress — about
125 k pixel writes, each one three address bytes and two colour bytes over
SPI1. Moving the selection is now roughly 3.5× less work, and no clear-to-black
flash.

`GUI_Invalidate()` forces a full repaint — call it after anything else has used
the screen (a JPEG, a video, the boot console).

Key handling: Button1/Button2 auto-repeat at 400 ms, then 130 ms, then 60 ms
after two seconds; held on, a step is 1 % of the folder after 5 s and 5 % after
10 s, five steps a second. Button3 opens on a short press, goes back on a press
of 500 ms or longer.

A folder may hold 10 000 entries (`README_MUSIC.md` §9). One with two songs or
more has a `SHUFFLE PLAY` row after `[ BACK ]`.

---

## 4. MKV seeking

### Timeline

`MKV_Open_Timeline()` parses `TimecodeScale` and `Duration` from the Info
element and locates the first cluster. If `Duration` is missing — streamed or
piped files often have none — `MKV_Duration_From_Tail()` scans the last 3 MB
for the final cluster timecode and adds a 2 s guard. On a 45 s test file that
fallback estimated 46 s, which is close enough for a progress bar.

With no timeline at all, seeking falls back to stepping through the file by
byte percentage and the bar shows file position instead of time.

### Seeking

`MKV_Seek_To_Ms()` starts from the file's average byte rate, then corrects from
where the probe actually landed, using the bitrate **measured locally** —
`MKV_Local_Rate()` takes the probed cluster's byte span and the next cluster's
timecode, which describes the neighbourhood far better than the file average.
A `[lo,hi]` bracket of clusters with known timecodes backs this up: if a
correction would jump outside the bracket, the search falls back to false
position inside it, so it always converges rather than drifting.

Every probe demands a cluster whose first block is a keyframe
(`MKV_Cluster_First_Block_Is_Key`), falling back to any cluster if none is
within the window. The result is biased to land at or just **before** the
target, never past it.

Measured on a 45 s H.264 file, every 5-second target landed on a keyframe
cluster exactly 1 s before the target, in a single probe.

### I-frame-only preview

The FPGA reader gates **every** SimpleBlock through `S_FRAME_WAIT`: it
releases one frame, then waits for `frame_done` and for the frame period to
elapse before taking the next. Nothing is consumed while it waits, so the
input FIFO backs up and DREQ stalls the PIC32.

A cluster in this material holds 25 SimpleBlocks and exactly one keyframe.
Pushing the whole cluster as a preview therefore cost **1000 ms** of forced
frame-wait per seek step — the single reason stepping felt like it had
stopped responding.

A preview now sends a synthetic single-frame cluster instead:

```
1F 43 B6 75 <size>   E7 81 00   A3 <size> <keyframe block>
```

`MKV_Find_Key_Block()` locates the SimpleBlock whose flags have bit 0x80 set;
`MKV_Build_Preview_Header()` writes the prologue with sizes that match exactly
what the PIC32 then sends. The reader parses it as an ordinary cluster holding
one keyframe, emits the IDR, and decodes one picture:

| | Old | New |
|---|---|---|
| Frames per seek step | 25 | 1 |
| Forced frame-wait | 1000 ms | 40 ms |
| Bytes per step (this file) | 7879 | 3857 |

Because the sizes are self-consistent, the reader's element bookkeeping stays
exact — `pos` reaches `cluster_end` precisely, `S_CLUSTER_CHILD` returns to
`S_SEG_CHILD`, and the next synthetic cluster is picked up at segment level.
No resync, and `file_resume` is not needed.

Two details matter for the reader to accept the block:

- the track-number vint is copied **verbatim**, because `S_BLOCK_CHECK_TRACK`
  matches it against `video_track_number` and skips anything else
- the block timecode is zeroed and the keyframe flag forced, so the reader's
  frame pacing sees a clean single frame. `fps_valid` latches once and is
  never recomputed, so synthetic timecodes cannot disturb the frame rate.

The reader already filters NAL types itself — only type 5 (IDR), plus type 1
when it is not waiting for an IDR — so the SEI NAL that rides along in the
first keyframe block is dropped by the FPGA and needs no handling here.

### Step size ramp

Continuous rather than a ladder: cubic in hold time from
`MKV_SEEK_STEP_MIN_MS` at a tap up to full speed at
`MKV_SEEK_HOLD_FULL_MS`.

The minimum step is **1000 ms**, which is about one cluster — the finest step
the file actually allows, since keyframes only exist at cluster starts. Asking
for less would land on the same cluster and show the same frame.

Full speed is **capped hard** at `MKV_SEEK_STEP_MAX_MS` (60 s), and also at
`duration/8`. Traversal speed comes from the repeat rate, not from the step
size: many small steps each showing a frame beats a few large jumps.

| Hold | 45 s clip | 5 min | 10 min | 45 min+ |
|---|---|---|---|---|
| tap | 2.8 s | 18.8 s | 30 s | 30 s |
| 2.5 s | 2.9 s | 19.0 s | 30.7 s | 32.3 s |
| 5 s | 3.2 s | 21.1 s | 35.6 s | 48.8 s |
| 7.5 s | 4.0 s | 26.7 s | 49.0 s | 93.3 s |
| 10 s+ | 5.6 s | 37.5 s | 75 s | 180 s |

A tap is `MKV_SEEK_STEP_MIN_MS` — **30 s** — and full speed is capped at
`MKV_SEEK_STEP_MAX_MS`, three minutes. Two further limits keep short files
sane: `duration/8` caps the top, and `duration/MKV_SEEK_MIN_DIVISOR` scales the
tap down, so a 45 s clip taps 2.8 s rather than swallowing itself in one press.

At about eight steps a second, a 45 minute film crosses in roughly two seconds
of holding and a two hour film in five.

### The filesystem was the bottleneck

A seek step measured about five seconds on hardware, fifty times what the
read counts predicted. The reason was not in the MKV or preview code at all.

**`FS_FAT32_NextCluster()` read a 512-byte sector from USB for every single
cluster step.** A FAT sector holds 128 consecutive chain entries, so walking
a chain re-read the same sector 128 times over. `FS_Fat_Sector()` now keeps
one FAT sector in RAM - 512 bytes of state.

**`FS_FAT32_MapPos()` walked the chain from the first cluster on every
backward jump.** Its cache only moved forwards, so stepping back through a
100 MB file meant over a thousand lookups each time. There is now a sparse
map of the chain (`FS_CHAIN_SLOTS`, 64 entries) filled in as the chain is
walked, so it costs nothing extra, plus a fast path for a file laid down in
one piece, which needs no walk at all. exFAT gets the same treatment; its
no-FAT contiguous case was already direct.

Modelled on a 107 MB file with 32 KB clusters and a chain that fragments
every 97 clusters:

| | lookups | USB reads before | USB reads now |
|---|---|---|---|
| First seek to 53 MB | 1617 | 1617 | 33 |
| Backward to 40 MB | 76 | ~1200 | 2 |
| Backward to 30 MB | 83 | ~900 | 3 |
| 20 scattered seeks | 1233 | ~20000 | 25 |

At a few milliseconds per USB read that is the difference between seconds
and a few milliseconds.

### The position walked backwards and forwards

`MKV_Seek_To_Ms()` is a bracketing search: it minimises the distance to its
target and has no notion of which way the user asked to go. On variable
bitrate material its best match for "one keyframe on" can be the keyframe
already on screen, or an earlier one, so the position oscillated instead of
stepping.

The session now keeps the committed position separate from the candidate,
and a candidate that did not move the requested way is replaced by an exact
hop - `MKV_Next_Key_Cluster()` forwards, `MKV_Key_Cluster_At_Or_Before()`
backwards - both directional by construction.

### Making a step cheap

Four things dominated a seek step once the preview was decoding. They are
all gone or much smaller:

**The repeat delay.** `MKV_SEEK_REPEAT_MS` was 120 ms, sized when a step was
nearly free. It is **0** now - decoding and drawing the preview paces the
loop on its own and the release is debounced separately, so any wait here
was dead time. The pause loop compiles out entirely.

**Reading the keyframe in 256-byte chunks.** That was about a hundred
separate USB transfers for a 25 KB frame. `H264_Unescape_File()` now pulls
the whole NAL in with multi-sector reads straight into the slice buffer and
strips the emulation bytes in place, which is safe because unescaping only
ever removes bytes.

**Flushing the SPI after every pixel.** `GFX_Pixel` drains the receive FIFO
per call, which is right for a single dot and wrong 19200 times over.
`GFX_Blit_Row()` writes a whole row and flushes once.

**Seeking backwards read half a cluster.** Hopping cluster to cluster is
cheap because a cluster's size says where the next one starts, but only from
a boundary that is already known; an interpolated byte position lands in the
middle of a cluster, and finding its edge means reading the rest of the
payload. With megabyte clusters that was most of the cost. Boundaries are
now remembered as they are found (`MKV_Cache_Add`, 32 slots, evicting
whichever entry sits closest to another so coverage stays spread), and a
backward step hops from the nearest one behind its target. The cluster scan
also hops past a cluster that is valid but not a keyframe, instead of
byte-scanning through its payload.

Measured on the real movie, per step:

| | reads | KB read |
|---|---|---|
| Forward | 7 to 10 | 18 to 49 |
| Backward, before | 74 to 146 | 557 to 1161 |
| Backward, now | 5 to 33 | 27 to 48 |

Which works out at roughly:

| | before | now |
|---|---|---|
| Forward step | ~280 ms | ~86 ms |
| Backward step | ~720 ms | ~106 ms |

### Stepping by keyframe

A tap now moves exactly one keyframe rather than a fixed time, so every
picture the file can show is reachable. The spacing is measured from the
first pair the session lands on (`g_mkv_key_gap_ms`) - 10.24 s on the test
movie - and becomes the bottom of the step ramp, which still reaches three
minutes when the key is held. Forward that is a couple of cluster hops;
backward it comes from the boundary cache.

`MKV_SEEK_STEP_MIN_MS` (2 s) is only the floor used before the spacing has
been measured.

### Keeping the repeat rate up

Three things were costing time per step, all now gone or cheap:

**The preview push.** Gone from the seek path entirely — see above. That
removes both the 40 ms frame-wait the reader enforces per block and the bytes
it cost.

**The search on small forward steps.** `MKV_Walk_Forward()` walks cluster to
cluster from where the last step landed — a cluster's own size gives the next
cluster's offset, so each hop is a single 512-byte read. Used when the step is
at most `MKV_SEEK_WALK_MS` (6 s) and bounded by `MKV_SEEK_WALK_HOPS`; it
reports whether it reached the target, and the full search takes over if not.

**The fixed search margin.** The scanner only searches forwards, so it needs
some lead to land at or before the target. A fixed 1.5 s lead cost a whole
extra cluster on a fine step: ask for 1 s back, get 2 s. `MKV_Set_Seek_Margin()`
narrows it to half the step (floor `MKV_SEEK_MARGIN_MIN`, ceiling
`MKV_SEEK_MARGIN_MS`), and the refinement tolerance follows it down so a fine
step does not stop refining too early.

With no preview push, a step is just the cluster search plus the paint. The
cluster walk still applies to steps of 6 s or less, so it only fires on short
files where the tap is scaled down:

| | Per step |
|---|---|
| Cluster search | ~3 reads, ~9 KB |
| Cluster walk (small steps) | ~3 reads, ~1.5 KB |
| Paint | ~12 ms |
| Repeat pause | 120 ms |

Backward stepping has no walk path — MKV has no back-pointers — so it always
uses the search. The search margin is narrowed to half the step
(`MKV_Set_Seek_Margin`), which is what stops a fine step overshooting by a
whole cluster.

### Other constants worth knowing

| Constant | Default | Meaning |
|---|---|---|
| `MKV_SEEK_TOL_MS` | 1500 | Close enough; stop refining |
| `MKV_SEEK_PASSES` | 5 | Probes per seek |
| `MKV_KEY_WINDOW` | 3 MB | Scan distance looking for a keyframe cluster |
| `MKV_ANY_WINDOW` | 1 MB | Scan distance for the any-cluster fallback |
| `MKV_SEEK_STEP_MIN_MS` | 30000 | Step for a single tap |
| `MKV_SEEK_STEP_MAX_MS` | 180000 | Hard cap on a single step |
| `MKV_SEEK_HOLD_FULL_MS` | 10000 | Hold time to reach full speed |
| `MKV_SEEK_REPEAT_MS` | 120 | Pause between repeats; the whole cadence |
| `MKV_SEEK_LINGER_MS` | 1000 | Overlay stays up this long after release |
| `MKV_SEEK_DEBOUNCE_MS` | 30 | Release must persist this long to count |
| `MKV_SEEK_RESUME_MS` | 80 | Decode time allowed before switching back |
| `MKV_SEEK_WALK_MS` | 6000 | Largest step taken by cluster walk |
| `MKV_SEEK_MIN_DIVISOR` | 16 | Scales the tap down on short files |
| `MKV_SEEK_WALK_HOPS` | 12 | Bound on the walk |
| `MKV_SEEK_MARGIN_MS` | 1500 | Ceiling on the search lead |
| `MKV_SCAN_BLOCK` | 8 KB | Read size while scanning |

`OSD_PREVIEW_SETTLE_MS` in `OSD.h` is the one to adjust if previews feel
laggy; the preview itself is now a single frame, so there is nothing left to
cap.

---

## 5. On-screen display

`OSD.h` draws the bar from the reference image into y=186..239: position time
left, total duration right, current step size in the centre, a 296 px track
with the elapsed part in red and a red disc knob.

It appears on the first UP/DOWN press and disappears on release. `OSD_Hide()`
clears the strip to black and the next decoded frame paints over it.

The whole band is redrawn on each update (~8 ms) rather than erasing the old
knob, so the knob leaves no trail.

### Entering and leaving a seek

`MODE_VGA` selects which source the display scans out — the PIC32 frame
buffer at 0, the decoder at 1 — and never composites the two. The decisive
symptom was the bar showing **green, then flashing red**: green is the
decoder scanning out an all-zero YUV frame, red was the bar during the brief
moment `MODE_VGA` was low. Toggling per paint caused the flashing and meant a
preview frame could not be seen anyway.

So `MODE_VGA` drops once, on the way in, and is restored on the way out.

**On entry.** The frame buffer still holds whatever was drawn into it last
and is about to become the visible surface, so `OSD_Show` clears it before
painting the bar. That costs one full-screen fill.

**While held.** Nothing is streamed to the decoder, so a step costs only the
cluster search and `MKV_SEEK_REPEAT_MS` (120 ms) is the whole cadence.

**Before resuming.** The whole frame buffer is cleared once the seek ends,
so neither the bar nor the preview survives behind the video.

**On release.** The overlay lingers for `MKV_SEEK_LINGER_MS` (1000 ms) rather
than tearing down immediately, so tapping repeatedly stays inside one session
instead of rebuilding the screen between presses. A new press inside that
window restarts the step ramp from the smallest step. Releases are debounced
by `MKV_SEEK_DEBOUNCE_MS` (30 ms) so a contact bounce partway through a long
hold is not read as a release, which would otherwise reset the ramp.

**Soft resume.** Before the display switches back, the chosen keyframe is
pushed to the decoder while the overlay is still the visible surface, then
`MKV_SEEK_RESUME_MS` (80 ms) is allowed for it to be decoded. So the switch
lands on the new picture rather than flashing the stale one the decoder still
holds. `Play_MKV` then streams the real cluster from the same position and
playback carries on from that keyframe.

### Guaranteeing the resume point is a keyframe

`MKV_Seek_To_Ms` prefers key clusters but falls back to any cluster when none
is within the window, and resuming on one of those feeds the decoder
P-frames with nothing to reference. `MKV_Ensure_Key_Cluster()` checks the
landing and, if it is not a key cluster, looks forward for one — and if there
is none ahead, sweeps backwards for the nearest one behind.

The backward path matters on long-GOP material: in the final group there is no
further keyframe, so forward-only searching leaves the last part of such a
file unresumable. Measured on a 20 s clip encoded with `-g 250` (two
keyframes in the whole file), forward-only left 9 of 21 seek targets landing
on non-key clusters; with the backward sweep all 21 resolve.

| | seeks | already key | corrected | not key |
|---|---|---|---|---|
| test.mkv | 46 | 46 | 0 | 0 |
| fract.mkv | 7 | 7 | 0 | 0 |
| detail.mkv | 9 | 9 | 0 | 0 |
| lowq.mkv | 4 | 4 | 0 | 0 |
| longgop.mkv | 21 | 12 | 9 | 0 |

### What a real file exposed

Three faults only showed up on an actual movie, because every synthetic test
file was video-only and correctly muxed.

**Audio blocks were being taken for video keyframes.** In an interleaved file
the first SimpleBlock of a cluster is usually audio, and audio frames carry
the keyframe flag on *every* frame. So `MKV_Cluster_First_Block_Is_Key()`
returned true for every cluster, and `MKV_Find_Key_Block()` handed the
decoder an MP3 frame. Both now skip blocks whose track number is not the
video track (`MKV_Is_Video_Track()`, with the track number taken from
Tracks). Where the track is not yet known, any block still counts, which is
the old behaviour.

**Cluster child limits were far too small.** A video-only cluster holds a
handful of blocks; an interleaved one holds hundreds. The scan bounds were
16 and 64 children over a 4 KB window, which could not reach the video
keyframe. Now `MKV_KEY_PROBE` (64 KB) with 256 and 1024 children. Widening
those limits also turned up a latent bug: the loop counters were
`unsigned char`, so a limit of 256 would have looped forever.

**A truncated file's header lies about the duration.** The test movie's
Segment header claims 1.37 GB and 2890 s, but only 107 MB and 226 s are
present. Seeking with the header's duration sent every target to the end of
the file. `MKV_Open_Timeline()` now compares the Segment's declared end
against the actual file size; when it overruns, the duration is scaled by the
fraction of the body present. On the test movie that gives 227144 ms against
a true 226385 ms - **0.3% error, from the header alone**, with no scanning.
The tail scan remains only for files carrying no duration at all, because it
reads 3 MB and is far too slow to run at every file open.

### Software I-frame preview

Because `MODE_VGA` is a source select, a frame decoded by the FPGA cannot be
seen while the bar is up. Decoding the keyframe on the PIC32 instead puts the
preview into the same frame buffer the bar lives in, so it composites for
free with no RTL change. `H264_PIC.h` is that decoder — High profile, 4:2:0,
CABAC, 8x8 transform, I slices only, no deblocking. Verified pixel exact
against ffmpeg on 113 keyframes across five files, including all 23
keyframes of a real 226-second movie.

The SPS and PPS are not in the stream: for H.264 in Matroska they sit in the
video track's `CodecPrivate` as an avcC record, which `MKV_Open_Timeline()`
now pulls out while it walks the Segment (`MKV_Parse_TrackList`). The avcC is
split into SPS and PPS by `H264_Preview_Open()` when a file starts playing.

Per seek step `H264_Preview_Draw()` finds the keyframe SimpleBlock, walks its
length-prefixed NAL units for the IDR, unescapes it straight out of the file
into one buffer, decodes, and draws the result halved and centred above the
bar.

Measured cost per frame on the real bitstream — 36k CABAC decisions, 7k
bypass, 3634 IDCTs:

| | PIC32MZ @192 MHz |
|---|---|
| CABAC | 7.3 ms |
| Transforms | 5.0 ms |
| Prediction | ~4 ms |
| Blit to frame buffer | 24 ms |
| **Total** | **~40 ms** |

The blit dominates: `Memory_Wr` is five SPI bytes plus a MEMWR strobe per
pixel, 19200 times. An address auto-increment write mode in the FPGA would
let a row go out by DMA and remove most of it.

**Cost in RAM: about 177 KB** — 115 KB picture, 64 KB for the unescaped
slice, 5.5 KB of decoder state. `H264_PREVIEW 0` in `newmain.c` removes all
of it if the build needs the space.

`H264_RBSP_MAX` (64 KB) bounds the largest keyframe that gets a preview.
Real 320x240 material is far below it — the test files run 3 to 10 KB. A
frame that does not fit simply gets no preview and the bar carries on;
verified by a deliberately extreme crf 4 clip whose 78 KB frames are skipped
at 64 KB and decode correctly at 96 KB, so the limit is the only thing
stopping them.

### Hidden folders

`System Volume Information` and the other housekeeping directories the
operating systems leave on removable media are kept out of the browser two
ways, because either alone can miss:

- by attribute — an entry carrying **both** the hidden and system bits is
  skipped in the FAT32 and exFAT listers. Windows marks its OS folders that
  way and media files essentially never carry both, so the rule is specific
  without being broad enough to hide a file someone wanted.
- by name — `FS_NameIsSystemJunk()` catches `System Volume Information`,
  `RECYCLER`, `LOST.DIR` and `FOUND.000` case-insensitively, in case the
  attributes were lost across a copy.

`FS_NameVisibleForBrowser()` already dropped `$`-prefixed names, so
`$RECYCLE.BIN` was handled; it now also drops **any** leading-dot name rather
than just `.` and `..`, which covers `.Trashes`, `.Spotlight-V100`,
`.fseventsd` and AppleDouble `._` files.

### MODE_VGA is a source select, not a compositor

The decisive symptom was the bar showing **green, then flashing red**. Green is
the decoder scanning out an all-zero YUV frame; red was the bar during the
brief moment `MODE_VGA` was low. So `MODE_VGA` chooses *which* source the
display scans out — the PIC32 frame buffer at 0, the decoder at 1 — and never
composites the two. The bar and live video cannot share the screen.

Toggling it per paint was therefore wrong twice over: it caused the flashing,
and it meant a preview frame pushed during the seek could not be seen anyway.

`MODE_VGA` is now dropped **once**, when the bar appears, and restored when the
key is released. During the seek the screen is the frame buffer — black, since
`Browser_Play_File` clears it before playback — with a steady red bar and the
times. No flashing, no green.

### Previews, and landing on the I-frame

Since a preview cannot be seen while the bar is up, the per-step push is gone.
It bought nothing and cost a frame period per step in the reader's pacing.

Instead the session hands back the byte position of the keyframe cluster it
landed on, and `Play_MKV` resumes streaming the real cluster from there. The
first picture after the key is released **is** that I-frame, and playback
carries on from it. Which is the behaviour that was wanted; it just does not
need a preview to get there.

`MKV_SEEK_PREVIEW 1` in `newmain.c` restores the per-step push, along with
everything in `MKV_SEEK.h` that builds the synthetic single-frame cluster — for
a display path that can composite the overlay over video. That machinery is
still present and still verified; it is only switched off.

With nothing streamed during the seek, a step costs just the cluster search, so
`MKV_SEEK_REPEAT_MS` (120 ms) is the entire cadence.

### The flush is still needed

A seek key aborts the stream mid-buffer — `AVI_Send_Buffer_Polled` checks for
the abort every 16 bytes — so the reader is left partway through a SimpleBlock,
waiting for the rest of its payload. The cluster we resume with would be
swallowed as payload bytes.

`MKV_Cluster_Flush_Target()` finds the next cluster header — clusters are
contiguous, so that is the end of the one we are inside — and the session
streams the real bytes up to it before anything else. The reader takes them
exactly as it expected and comes back to segment level.

Verified by building the byte sequence a seek actually produces — sequential
playback aborted mid-block, the flush, then resume from a sought cluster — and
walking it the way `mkv_h264_reader.v` does:

| | Without flush | With flush |
|---|---|---|
| Clusters entered | 2 | 18 |
| Video blocks parsed | 29 | 450 |
| IDR NALs emitted | 2 | 18 |
| **Desyncs** | **1** | **0** |

Checked at nine abort offsets, including exactly on a cluster boundary: zero
desyncs in every case.

### If the colours are ever wrong again

The overlay uses the project's own constants — `Red` (0xF800), `Dark_Grey`,
`Black` — the same words the BMP and JPEG viewers write, nothing computed by
`RGB565()`. The red flash proved the pixel writes themselves were already
correct, so this is settled; the two diagnostics below are kept only in case
something changes.

`GFX_PIPELINE` defaults to **0**, sending every byte through `WriteSPI3()`
exactly as the original `Pixel()`/`Memory_Wr()` does. Since the flash came out
red, `GFX_PIPELINE 1` demonstrably works too and can be re-enabled; at a 120 ms
cadence the few ms it saves per update do not matter either way.

`OSD_COLOUR_TEST 1` in `OSD.h` paints the bar area as four labelled blocks —
R, G, B, W — drawn with `Red`, `Green`, `Blue`, `White`, so a field-layout
mismatch could be read straight off the screen.

## 6. `GFX_PIPELINE`

```c
#define GFX_PIPELINE 0      /* GFX.h - default */
```

`0` sends every byte through `WriteSPI3()`, identical to the original
`Pixel()`/`Memory_Wr()`. `1` queues the five bytes back-to-back and waits for
`SRMT` before strobing `MEMWR` — faster, but it gives the FPGA no inter-byte
gap, which is the prime suspect for the colour fault described above.
`GFX_BYTE_GAP` adds the gap back for anyone wanting to retry `1`.

`GFX_Flush()` drains the RX buffer and waits for `SRMT`. It is needed either
way: `IGNROV` is set, so a stale RX byte left by the stream DMA would make
`WriteSPI3()` return early.

---

## 7. Verification

`MKV_SEEK.h` was compiled natively and run against real ffmpeg output. Three
bugs surfaced that would all have presented on hardware as "seeking does not
work":

**Long-form EBML size vints.** ffmpeg pads Segment and Void sizes to a full
8-byte vint even when the value is small. The reader treated any vint longer
than 4 bytes as the "unknown size" marker, so the Segment child walk stopped at
the first Void and the timeline never parsed. Now the full payload is decoded
into 64 bits and only an all-ones value gets the sentinel. The same fix applies
to `MKV_Vint_Ram()`.

**CRC-32 ahead of the cluster timecode.** ffmpeg writes a CRC-32 element as the
first child of each cluster. The validator required `Timecode` first, so it
rejected every cluster in the file — the timeline parsed and then every seek
failed. It now steps over leading CRC-32 and Void elements.

**Non-monotonic speed ladder.** Levels 7 and 8 had no fixed-time entry and fell
back to percentage only, so the step *shrank* past level 6 — holding longer
would have seeked slower.

**Whole-cluster previews.** Not a parsing bug, a pacing one — see the
I-frame-only preview section above. Found by reading `S_FRAME_WAIT` in
`mkv_h264_reader.v`, not from the PIC32 side, where it is invisible.

The synthetic preview clusters are checked by building them with the real
`MKV_SEEK.h` code and then walking the resulting byte stream the way
`mkv_h264_reader.v` does — segment-level ID, `cluster_end`, cluster children,
`SimpleBlock`, track match, length-prefixed NAL walk with the reader's type
filter. On both test files every byte is accounted for: the child walk lands
exactly on `cluster_end`, and every preview yields exactly one IDR.

The full project, including the patched `USB_MSC_BARE.h`, compiles clean under
`gcc -std=gnu89 -Wall -Wextra`. The new files produce no warnings; the ones that
remain are pre-existing, in `FONT.h`, `FUNCTIONS.h` and `FS_GENERIC.h`.

### Known limit

A deliberately extreme test — 60 s of flat black concatenated with 60 s of
noise, a ~600:1 bitrate ratio — seeks accurately in the high-bitrate half but
parks at ~61 s for targets inside the black half. That 60 s of media is 0.15 %
of the file, and five probes cannot cross it. Real material does not do this;
raising `MKV_SEEK_PASSES` would fix it at the cost of USB reads per keypress.

---

## G3D scenes

`.g3d` files (3D scenes for the FPGA renderer, built by `gfx_cow.py`) show in
the browser with their own icon: the MKV page with a cube and a G3D label
(`g3d_icon_bmp` in `ICON.h`). The type is `VFS_TYPE_G3D`, detected by the
extension in any case.

Enter on a G3D file raises `MODE_VGA` and streams the file unchanged, with the
same DREQ pacing as video (`Play_G3D()` in `newmain.c`). Loading ends at the
end of the file, or when DREQ stays low for `AVI_DREQ_TIMEOUT` because the
FPGA holds the stream. Unlike the players, the browser does not come back
then: the 3D view stays on screen until Enter is pressed again. Enter while the
file is still loading cancels straight back to the browser.

With `MODE_VGA` high the FPGA must route the SPI stream to `GFX3D_TOP` and show
its 640x480 frame buffer.

---

## Music

MOD, S3M, XM and IT modules and MP3, FLAC and WAV recordings show with their
own icons and play on the FPGA. Enter on a song plays it and then the rest of
the folder: when a song ends the next one starts, after the last one the
first; `SHUFFLE PLAY` plays the folder in random order. While playing, DOWN
and UP go to the next / previous song, Enter goes back to the browser with the
cursor on the last song played.

`MUSIC.h` converts a module into a fixed SDRAM layout and streams it with the
G3D chunk format; `AUDIO.h` streams a recording as it plays, into a ring the
FPGA decodes from. The FPGA reports the end of a song by holding DREQ low for
50 ms. `README_MUSIC.md` has the layouts and the protocol.

Two `FS_GENERIC.h` read bugs turned up on the way and are fixed, and folder
listing was rewritten for large folders and NTFS — see `README_MUSIC.md` §9,
§10. Video seeking goes through the same code.
