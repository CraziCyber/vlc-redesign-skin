# VLC Redesign skin

A skin for VLC 3 based on Juxtopposed's redesign of VLC.

- Design: [VLC Redesign on Figma Community](https://www.figma.com/community/file/1691465999134370980/vlc-redesign)
- Video: [I Redesigned VLC After 25 Years](https://www.youtube.com/watch?v=ERFeOgqk324)

![VLC playing a video with the skin](docs/screenshots/video.png)

## Install

This installer requires **VLC 3.0.24 (64-bit) on Windows**.

1. Clone this branch and close VLC completely.
2. Open `dist/Install-VLC-Subtitles.cmd` from the cloned folder. Approve the administrator prompt if Windows displays it.
3. Open VLC from your usual shortcut and play a file. The **Audio** and **Subtitles** buttons open their respective tabs in the track panel. Use **Video**, **Audio** and **Subtitles** to select embedded tracks, or choose an output device under **Audio**. The video and subtitle lists show up to ten tracks at a time and scroll to the rest.

The installer adds the subtitle module to VLC and installs and selects `VLC-Redesign.vlt`. It backs up existing VLC settings before changing them. Run the installer again to update an existing setup.

### Search and download from OpenSubtitles

Play a video, open **Subtitles → External Tracks → +**, and select **From OpenSubtitles**. The search field starts with the video filename without its extension. Press Enter or click the magnifying glass to search for an edited title. The language defaults to English.

On first use, create an API key in your OpenSubtitles.com account. Paste it into the masked field in VLC and select **Save API Key**. VLC stores it in Windows Credential Manager for your Windows account. [OpenSubtitles API Consumers](https://www.opensubtitles.com/en/consumers)

For **To This File**, each subtitle filename appears on its own line. Hover over a truncated name to read it in full. Use **Load More** to see additional results. Select a result and choose **Save** to download and activate it under **External Tracks**.

For **To This Folder**, each video from the current video's folder appears alongside its suggested OpenSubtitles match. For a series, the default query shows the season and the search matches each episode by its number. Click the subtitle side of a row, or select the row and press Enter, to cycle through available matches. Scroll to see any remaining files. **Save** adds the chosen matches for the folder, starting with the video that is playing. If the download allowance runs out, VLC adds the subtitles it already fetched and shows how many were added.

Downloaded `.srt` files are cached in `%LOCALAPPDATA%\VLC Redesign\OpenSubtitles` so VLC can reuse them. The selected tracks are applied for the current VLC session.

**To This Playlist** displays the VLC playlist in order, with track titles and available artist names. Its source menu offers **From Computer** and **From Link ...**.

### Add local subtitle files

**From Computer** suggests matching `.srt` files near the media, including the `Subs` and `Subtitles` subfolders. Exact names appear first. Choose **Browse...** to pick any `.srt` in the Windows file picker. For a folder or playlist, select the media row before browsing and click its subtitle to cycle through suggestions.

After **Open**, the selected file appears in the list. Choose **Save** to apply the selections during the current VLC session and return to **External Tracks**. Click an added track to use it. The checkmark follows the active subtitle track. VLC uses the subtitle from its original location.

You can also drag `.srt` files from File Explorer onto the player during playback. The files appear under **External Tracks**, with a checkmark beside the active subtitle.

Right-click the player to open the redesigned menu. Use the arrow keys and Enter to navigate, or press Esc to close it. The menu shows the shortcuts configured in VLC. **Hide UI** slides the player controls away in the current window. Select **Show UI** to slide them back, or use `Ctrl + \` if that combination is unassigned in VLC.

The gear in the playback controls opens a four-tab preferences sidebar. Click the gear again or click outside the sidebar to close it. VLC's preferences are under **Settings** in the ••• menu at the top left.

Under **Defaults** in the gear's **Subtitles** tab, enter a **Subtitle Track** number (starting at `1`) and a **Subtitle Language** such as `English` or `en`. Press Enter to save. The preset chooses the numbered track if it matches the language, or the first track in that language. If the language is unavailable, VLC keeps its usual selection. Use **Auto** for the track and **Any language** for the language to clear the preset. A manual switch applies to the current video.

Under **Synchronization**, drag the sliders or type a value and press Enter. **Subtitle Speed** retimes external SubRip (`.srt`) and MicroDVD (`.sub`) tracks during playback. Set it to `1.000 fps` for automatic timing. Use `0` for **Subtitle Duration Factor** to turn the duration filter off.

Scroll to **Appearance** to set the font, colors, offset, size and outline, background opacity, and embedded formatting for supported text tracks. **Font Size** resizes text rendered by VLC's FreeType renderer while the cue is visible. The first **Pixels** value is calculated from the **Relative** size and the displayed video. With no video visible it uses a 720 px reference height. VLC saves the appearance choices. Reopen the video when the sidebar asks you to apply changes that the current renderer has cached.

## What's included

- The player, in dark and light
- A playlist sidebar
- A mini player
- An equalizer in the preferences sidebar under **Audio**
- Controls in fullscreen

The ••• menu switches theme and opens the mini player and playlist.
More [screenshots](docs/screenshots).

## Not included

A skin can only restyle VLC's player window. Settings, dialogs and menus keep VLC's own look,
and the design's library views don't exist in VLC 3. [docs/notes.md](docs/notes.md) has the details.

## Building

`python build.py` regenerates `dist/VLC-Redesign.vlt` and its unpacked files. It needs Python 3 with Pillow and fontTools.
[docs/notes.md](docs/notes.md) explains how to build the skin and the Windows subtitle module.

## Credits and license

- **Design:** ["VLC Redesign"](https://www.figma.com/community/file/1691465999134370980/vlc-redesign) by Juxtopposed, licensed under
  [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). This skin adapts it for VLC 3. The
  icons are cut from the design; the images in `dist/`, `docs/screenshots/` and `reference/figma/`
  are derived from it and shared under the same license.
- **Font:** [Inter](https://github.com/rsms/inter) by the Inter Project Authors, under the SIL Open
  Font License ([fonts/Inter-LICENSE.txt](fonts/Inter-LICENSE.txt)). The playlist uses a modified
  copy, renamed Inter Rows.
- **Code:** `build.py`, `skinlib/` and `tools/` are dedicated to the public domain under
  [CC0 1.0](LICENSE). CC0 covers only the code: the design and Inter keep the licenses above.

This is an unofficial skin, not affiliated with or endorsed by VideoLAN. VLC and the VLC cone are
trademarks of VideoLAN.
