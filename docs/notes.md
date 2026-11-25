# Notes

How the skin maps the Figma design, what VLC's skin engine (skins2) cannot do, and what
building it taught. The [README](../README.md) is the short version.

## Building

```bash
python build.py
```

This regenerates `dist/VLC-Redesign.vlt` and the same files unpacked in `dist/VLC-Redesign/`.
It needs Python 3 with Pillow and fontTools (lxml too, for full DTD validation). The skin bundles Inter, the design's typeface, from `fonts/`
(SIL Open Font License, see `fonts/Inter-LICENSE.txt`), because skins2 only loads fonts shipped
inside the skin.

With the Windows subtitle module installed, you can try the build in a separate VLC instance.

```bash
"C:\Program Files\VideoLAN\VLC\vlc.exe" --ignore-config --no-one-instance -I skins --extraintf=subtitlepanel --skins2-last="C:\dev\vlcskin\dist\VLC-Redesign.vlt"
```

### Windows subtitle test player

`python tools/build_subtitle_player.py` builds a portable copy of 64-bit VLC 3.0.24 in
`testplayer/`. It uses MinGW-w64 from MSYS2. Use `--vlc`, `--toolchain` and `--output` if
the installation, compiler or destination is in a different folder. Open
`testplayer/Start-VLC-Subtitle-Test.cmd` to use the live Video, Audio and Subtitles tabs.

The interface module reads VLC's video, audio and subtitle choices and checks the track IDs against
the metadata of local movie files. For sources without readable metadata it uses
VLC's current choices. **None** disables the selected track type. The video and subtitle lists
show up to ten tracks at a time. The **Audio** tab lists output devices reported by VLC's active audio output.

## What is in it

| Figma | In the skin |
| --- | --- |
| Player (247:445), Controls (491:1779) | Resizable main window with a 40 px top bar, rounded video area and 54 px control bar with seek fill, wedge volume (0-200 %, 100 % in the middle, as VLC maps it), audio tracks, subtitles, preferences and playlist buttons. |
| Player - Music (4767:23726) | With audio and no video, the cover is centred above the title; without a cover, just the title. With nothing to show, the video area is black, as in VLC. |
| Player with sidebar (187:328), sidebar (848:1996) | Playlist layout: a rail (open files, folder, disc, network stream, media info, preferences) and a panel with shuffle, repeat (all / one), sort, load, save, remove, add, and the playlist itself, with the playing item in orange. |
| Player - Light (2425:13624) | Every layout also comes in light. The "..." menu switches the theme for the player, track panel and fullscreen bar. |
| Miniplayer (970:3090) | 391 × 339 mini player: video or cover, progress, title, controls and time. |
| Audio - Equalizer (6114:13610), Toggle (536:1014) | The Audio tab in the preferences sidebar has equalizer presets, preamp and 10 bands with sliders and editable dB values. |
| Player - hide UI (3328:11554) | Hide UI slides the title bar up and the controls down inside the player window. Fullscreen remains a separate action. |
| menu (6062:13354), right-click menu (6065:13616) | The "..." menu opens media, view and settings. The Windows module draws the rounded right-click menu with the original icon sheet and displays configured VLC shortcuts. |
| Icon set (42:356) | Every icon, cut from the sheet at 1×. |

Screenshots of each are in [screenshots](screenshots).

## What a skin cannot do

skins2 is VLC 3's skin engine. It draws the player's own windows and nothing else, so:

- **Library views, onboarding, the settings pages, keyboard shortcuts and the open/network
  dialogs** are VLC's Qt windows and keep VLC's look. The skin opens them; it cannot restyle them.
- **Skin-defined menus** use native Windows styling. The Windows module supplies the right-click menu and delegates playback and dialog actions to VLC.
- **Tooltips** are drawn by VLC in fixed colours (pale yellow `#FFFFD0`, black border and text,
  `src/tooltip.cpp`). The skin only picks their font (`tooltipfont`, here Inter 12).
- **Translucency** fails (see below), so the music view has no blurred backdrop, and the cover
  has square corners.
- **±10 s skip buttons and the waveform seek bar** from the music controls: skins2 has no
  seek-by action, and no access to the audio to draw a waveform from.
- **The breadcrumb** shows the media's folder in Figma; skins2 does not know it, so the button
  opens media instead. The playlist has no item counts or thumbnails, and VLC's
  "Media Library" node shows up as a row.
- **Windows resize only from the right edge, the bottom edge and that corner**: those are the
  only resize actions skins2 has.

## How it is built (and what skins2 taught)

- `build.py`: the layouts. The main player has dark and light layouts for the full player,
  playlist and mini player. The full player and playlist also have preferences sidebar layouts.
  The fullscreen preferences layouts hold the selected tab for the main player as well.
- `skinlib/`: the theme.xml builder (`skin.py`, `xmlgen.py`), the drawing helpers and Figma
  icon map (`paint.py`), and the colours (`palette.py`).
- `reference/figma/`: the Figma screenshots and page metadata the measurements came from.
  `tools/figtree.py <node-id>` prints any node's layer tree from it.
- VLC's own default skin (`skins/default.vlt` in VLC's install folder, a gzipped tar) is the
  most reliable example of the format; it is not copied here, as it isn't ours to redistribute.
- `tools/startup_time.py`: times how long VLC takes to show a skin's window.
- `tools/sheet_stats.py`: sizes of the sprite sheets and how much of each is used.
- `tools/run_skin.py`: launches VLC with the skin (`--ignore-config`, so your settings are not
  touched), drives it through the rc interface and saves screenshots of VLC's windows only.
  `build.py --start light_side --preview-fsc` sets up the views to capture.
- The build is reproducible: building twice gives byte-identical files in `dist/`, so a
  commit only shows `dist/` changing when the skin did.

## Checking it works

1. **Every build** runs `tools/check_theme.py` and stops if it finds a problem. It validates
   theme.xml against VLC's own `skin.dtd` (fully with lxml installed, a smaller check without)
   and checks ids are unique. skins2 itself
   only logs a warning and leaves the control dead, so the checker makes sure every image,
   font, action, variable and text token is one skins2 knows (lists from VLC 3.0's source),
   and that every control lies inside its layout.
2. **`python tools/smoke_test.py`** runs the skin in VLC (`--ignore-config`) and checks
   play/pause, seeking and volume by clicking, mute, next/previous, the music cover, the
   audio and subtitle track buttons, shuffle and the repeat cycle, sidebar open/close,
   maximize/restore, expanding the mini player, the "..." menu's playlist, mini player and
   theme items, and quitting. It clicks by posting mouse
   messages to VLC's window (and keystrokes to its menus), so the real cursor never moves, reads VLC's state from its HTTP
   interface, and fails on any skins2 warning in VLC's log. Screenshots of each step land in
   `shots/smoke/`. Create its media first with `python tools/make_testmedia.py` (needs ffmpeg).
   Leave the PC alone while it runs (about two minutes): native menus close as soon as another
   window takes focus, so using the PC meanwhile makes the menu checks fail.
3. **`python tools/edge_cases.py`** saves screenshots to look over: the player shrunk to its
   minimum size, a title too long to fit (it scrolls, as VLC skins do), and a 30-item
   playlist.
4. **By hand**, what the test cannot reach:
   - the rest of the "..." menu and the preferences sidebar
   - fullscreen and its floating bar, which appears on mouse movement
   - dragging the window, resizing it from the right and bottom edges, and minimizing
   - dragging files onto the window, tooltips, and scrolling a long playlist
   - the subtitles button and the dialogs it and the rail open
   - that the equalizer sliders change the sound

None of this is in the skins2 documentation. Items 1–3 and 8 were measured in VLC 3.0.24, item 9 was seen in use;
items 4–7 come from VLC's 3.0 source (`modules/gui/skins2`) and are confirmed by the skin working:

1. **Partly transparent pixels render dark.** Image and button controls are drawn into their
   own buffer as if over black, then copied into the window, so a pixel shows as
   `colour × (alpha/255)²` with nothing of what lies beneath. Measured: white at alpha 64
   renders as 16, at 128 as 64. Every image is therefore composited onto the surface it sits
   on (`paint.solidify`), keeping only fully transparent pixels. `Skin.bitmap` refuses any
   image that has partly transparent pixels. Text controls blend correctly.
2. **Checkboxes can show the opposite of their state.** A Checkbox flips its image on every
   change notice instead of reading the variable, so one that starts out of step stays wrong.
   `vlc.isMute` did. Every on/off control here is two buttons whose `visible` reads the variable.
3. **Sliders have no fill.** The seek bar fill is the cursor of a second slider on the same
   curve: a strip that ends at the handle. skins2 centres a cursor on the value, so the strip
   is the left half of an image whose right half is empty, which is why the second sprite sheet
   is 9,011 px wide and half transparent. An opaque image covers the part that overhangs left of
   the track, and the real handle slider sits on top, where it takes clicks. See `progress()`
   in `build.py`. The volume wedge uses 90 frames of a SliderBackground instead, laid out 9 x 10.
4. **A window opens on the layout defined last**, not the first.
5. **A slider's size comes from its curve.** It is the largest x and y of the points plus one,
   counted from zero. A SliderBackground is scaled by the ratio of the control's size to that
   size, so curves are placed so the ratio stays 1.
6. **Playlist rows put their text at the bottom of the row**, with the font's descender as the
   only gap below it, so in a roomy row the text sits low. The playlist therefore uses
   "Inter Rows", a copy of Inter that `paint.rows_font` writes at build time with a deeper
   descender, which centres the text in its 32 px row. The row icon's image (36 px wide, so the
   text lines up with "Add media") is centred the same way.
7. **An Image's `action` can only be `move` or a resize.** Other commands work only as `action2`
   (double-click), which is why the empty video area opens media on double-click.
8. **Every image file costs startup time.** VLC unpacks a `.vlt` into a temp folder on every
   start and decodes each image on its own. With ~470 separate PNGs, VLC took 8 s to show the
   skin; VLC's default skin takes 0.3 s. `Skin` now packs all images into two sprite sheets
   (as `SubBitmap`s), with identical images stored once, and the skin starts in 0.4 s.
   `tools/startup_time.py` measures this. The sheets are packed tightly (skyline packing,
   93% of the main sheet used): VLC keeps them decoded in memory, and a first, simpler
   packing left 62% empty and cost 27 MB.
9. **Clicks only hit opaque pixels.** A button whose normal image is a bare icon on a transparent
   background can be clicked only on the icon's strokes (three dots, for "..."). Button images
   are therefore filled with the colour of the surface they sit on (`Art.get(opaque=True)`).
