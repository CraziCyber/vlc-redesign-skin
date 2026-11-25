#!/usr/bin/env python3
"""Build the VLC Redesign skin for VLC 3.0 (skins2) from the Figma reference.

    python build.py

Writes dist/VLC-Redesign/ (theme.xml, img/, fonts/) and dist/VLC-Redesign.vlt.
Run dist/Install-VLC-Subtitles.cmd to install the subtitle module and select this skin.

docs/notes.md explains what of the Figma design is in the skin and what skins2 cannot do.

Every image is drawn knowing the surface it will sit on (`on=`), because skins2 cannot
blend partly transparent pixels with what is beneath them; see paint.solidify.
"""
import argparse
import gzip
import shutil
import tarfile
import tempfile
from pathlib import Path

import sys

from skinlib import paint as P
from skinlib.context_menu import add_skin_commands
from skinlib.palette import DARK, LIGHT, THEMES
from skinlib.skin import Layout, Skin

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / "tools"))
import check_theme  # noqa: E402  (tools/ is not a package)

NAME = "VLC-Redesign"
ICONS = P.Icons()

# The widest seek track whose fill can follow the handle (see progress()): a track this
# wide belongs to a 5,190 px wide window. Longer only costs memory in VLC.
MAX_TRACK = 4500
PREFERENCES_WIDTH, PREFERENCES_TOP, PREFERENCES_BOTTOM = 444, 40, 54
PREFERENCES_HEIGHT = 720 - PREFERENCES_TOP - PREFERENCES_BOTTOM
PREFERENCES_MARGIN, PREFERENCES_TABS, PREFERENCES_HEADER = 10, 4, 74
PREFERENCES_BODY_LEFT, PREFERENCES_BODY_RIGHT, PREFERENCES_BODY_BOTTOM, PREFERENCES_CORNER = 22, 14, 24, 10
PREFERENCES_TAB_TOP = 12

STOPPED, ACTIVE = "vlc.isStopped", "not vlc.isStopped"
ART_VIEW = "not vlc.isStopped and not vlc.hasVout"


def lid(t, kind):
    """Prefix a layout kind with its theme name."""
    return f"{t['name']}_{kind}"


def other(t):
    return LIGHT if t is DARK else DARK


def switch_theme(t, kind):
    o = other(t)
    return (f"main.setLayout({lid(o, kind)});"
            f"subtitlewin.setLayout({lid(o, 'subtitles')});"
            f"fullscreenController.setLayout({lid(o, 'fsc')})")


class Art:
    """Theme-bound bitmap factory. Ids are prefixed with the theme name; `on` names the
    palette colour of the surface an image sits on, and its edges are composited onto it."""

    def __init__(self, skin, t):
        self.skin, self.t = skin, t

    def get(self, key, make, on=None, opaque=False):
        """opaque fills the whole image with the surface colour, not just its edges: skins2
        clicks only hit opaque pixels, so a button drawn as a bare icon is clickable only
        on the icon's strokes."""
        def draw():
            img = make() if callable(make) else make
            if on:
                return (P.flatten if opaque else P.solidify)(img, self.t[on])
            return img
        return self.skin.bitmap(f"{self.t['name']}.{key}{'@' + on if on else ''}{'.o' if opaque else ''}", draw)

    def px(self, color_key):
        """A 1 x 1 pixel of a palette colour, for scaled fills."""
        return self.get(f"px.{color_key}", lambda: P.solid(1, 1, self.t[color_key]))

    def states(self, key, draw, specs, on):
        """Three images (up, over, down) from draw(*spec) for each spec in specs."""
        return tuple(self.get(f"{key}.{name}", lambda s=spec: draw(*s), on, opaque=True)
                     for name, spec in zip(("up", "over", "down"), specs))

    def icon_button(self, icon, w=30, h=28, ix=3, iy=2, r=6, active=False, on="chrome",
                    *, original_colors=False):
        """A flat icon button: just the icon, with a rounded highlight on hover.
        Active buttons draw the icon in the accent colour. Original colours preserve
        the two-tone artwork in every button state."""
        t = self.t

        def draw(bg, color):
            img = P.blank(w, h)
            if bg:
                img.alpha_composite(P.rrect(w, h, (0, 0, w, h), r, t[bg]))
            glyph = ICONS.raw(icon) if original_colors else ICONS.tint(icon, t[color])
            img.alpha_composite(glyph, (ix, iy))
            return img
        fg = ("accent", "accent_hover", "accent_down") if active else ("icon", "icon_hover", "icon_down")
        key = f"ib.{icon}.{w}x{h}.{ix}.{iy}{'.on' if active else ''}"
        if original_colors:
            key += ".raw"
        return self.states(key, draw, ((None, fg[0]), ("hover_bg", fg[1]), ("down_bg", fg[2])), on)

    def accent_button(self, icon, w, h, r=6, on="chrome"):
        """The orange play/pause pill."""
        t = self.t

        def draw(bg):
            img = P.rrect(w, h, (0, 0, w, h), r, t[bg])
            img.alpha_composite(ICONS.tint(icon, t["on_accent"]), ((w - 24) // 2, (h - 24) // 2))
            return img
        return self.states(f"ab.{icon}.{w}x{h}", draw, (("accent",), ("accent_hover",), ("accent_down",)), on)

    def window_button(self, icon, danger=False, on="chrome"):
        """Title-bar buttons (36 x 24). Close turns red on hover, as on Windows."""
        t = self.t

        def draw(bg, color):
            img = P.blank(36, 24)
            if bg:
                img.alpha_composite(P.rrect(36, 24, (0, 0, 36, 24), 5, bg))
            img.alpha_composite(ICONS.tint(icon, color), (6, 0))
            return img
        over_bg = t["close_hover"] if danger else t["hover_bg"]
        over_fg = "#FFFFFF" if danger else t["icon_hover"]
        down_bg = P.mix(t["close_hover"], "#000000", .15) if danger else t["down_bg"]
        return self.states(f"wb.{icon}", draw, ((None, t["icon"]), (over_bg, over_fg), (down_bg, over_fg)), on)

    def labelled_button(self, key, icon, label, w, h, ix, iy, tx, base, size, on,
                        colors=("icon", "icon_hover", "icon_down"), *, icon_color=None,
                        original_colors=False):
        """An icon followed by text, e.g. the top bar's 'Open media'."""
        t = self.t

        def draw(bg, color):
            img = P.blank(w, h)
            if bg:
                img.alpha_composite(P.rrect(w, h, (0, 0, w, h), 6, t[bg]))
            glyph = ICONS.raw(icon) if original_colors else ICONS.tint(icon, t[icon_color or color])
            img.alpha_composite(glyph, (ix, iy))
            return P.text(img, tx, base, label, size, t[color])
        return self.states(key, draw, ((None, colors[0]), ("hover_bg", colors[1]), ("down_bg", colors[2])), on)

    def handle(self, w, h, r, colors, on, key):
        t = self.t
        return self.states(f"{key}.{w}x{h}", lambda c: P.rrect(w, h, (0, 0, w, h), r, t[c]),
                           tuple((c,) for c in colors), on)

    def nothing(self):
        return self.skin.bitmap("transparent", lambda: P.blank(1, 1))


def progress(L, art, *, x0, x1, y, h, inset, r_track, r_fill, handle, anchor_l, anchor_s,
             bg, on="chrome", handle_on="fill", fill_key="fill",
             handle_keys=("handle", "handle_hover", "accent_down"),
             value="time", tip="$T / $D", visible=ACTIVE, draw_track=True, name="seek"):
    """A track with a fill that follows the slider value, as in the Figma seek bar.

    skins2 sliders have no fill, so this stacks three things:
      1. a slider whose cursor is a very long strip of fill colour ending at the handle
         (it reaches far to the left, past the start of the track),
      2. an opaque image that repaints whatever lies left of the track, hiding that overflow,
      3. a second slider on the same curve with the real handle; being on top, its
         background takes the clicks, so clicking the track seeks as usual.

    x0/x1: track edges (x1 exclusive) in layout coords; y: track top; h: track height.
    handle: (w, h, r). bg(x0, y0, x1, y1) returns the opaque image under a layout rectangle.
    on: surface around the track. anchor_l pins things left of the track; anchor_s is for
    the parts that stretch with it.
    """
    t = art.t
    hw, hh, hr = handle
    fh = h - 2 * inset
    lo = x0 + inset + hw // 2                 # handle centre at value 0
    hi = x1 - inset - (hw - hw // 2)          # ... and at value 1
    length = hi - lo
    cy = y + h // 2                           # centre line of track, fill and handle

    if draw_track:
        cap = r_track
        whole = lambda: P.rrect(cap * 3, h, (0, 0, cap * 3, h), r_track, t["track"])
        L.image(art.get(f"{name}.cap.l.{h}.{r_track}", lambda: whole().crop((0, 0, cap, h)), on),
                x0, y, anchor=anchor_l, hint=f"{name}track")
        L.image(art.get(f"{name}.mid.{h}", lambda: P.solid(1, h, t["track"])), x0 + cap, y,
                w=x1 - x0 - 2 * cap, h=h, resize="scale", anchor=anchor_s, hint=f"{name}track")
        L.image(art.get(f"{name}.cap.r.{h}.{r_track}", lambda: whole().crop((cap * 2, 0, cap * 3, h)), on),
                x1 - cap, y, anchor="br" if anchor_s == "bottom" else anchor_l, hint=f"{name}track")

    # 1. the fill: a cursor whose centre column sits on the value and whose strip ends at
    #    the handle's right edge. skins2 centres a cursor on the value, so the strip is the
    #    left half of an image whose right half is empty. It must reach back to the start of
    #    the track (the mask hides any more); a stretching track can grow up to MAX_TRACK.
    reach = (MAX_TRACK if anchor_s != "tl" else length) + hw // 2 + 1

    def strip():
        img = P.blank(2 * reach + 1, fh)
        img.paste(P.solid(reach + hw - hw // 2, fh, t[fill_key]), (0, 0))
        return img
    L.slider(value, lo, cy - fh // 2, f"(0,{fh // 2}),({length},{fh // 2})",
             up=art.get(f"fill.{fh}.{hw}.{fill_key}.{reach}", strip), anchor=anchor_s, visible=visible,
             hint=f"{name}fill")

    # 2. the mask over the overflow left of the track, with the fill's rounded start
    fy = cy - fh // 2

    def mask():
        w = x0 + inset + 40
        img = bg(0, y, w, y + h)
        img.alpha_composite(P.rrect(w, h, (x0, 0, w + 40, h), r_track, t["track"]))
        img.alpha_composite(P.rrect(w, h, (x0 + inset, inset, w + 40, h - inset), r_fill, t[fill_key]))
        return img.crop((0, fy - y, lo, fy - y + fh))
    L.image(art.skin.bitmap(f"{L.id}.{name}.mask", mask), 0, fy, anchor=anchor_l, visible=visible,
            hint=f"{name}mask")

    # 3. the interactive handle, on the same curve but centred on its own height
    up, over, down = art.handle(hw, hh, hr, handle_keys, handle_on, key=f"{name}.handle")
    L.slider(value, lo, cy - hh // 2, f"(0,{hh // 2}),({length},{hh // 2})", up=up, over=over, down=down,
             thickness=max(h // 2, hh // 2), tip=tip, anchor=anchor_s, visible=visible, hint=name)


# --- the control bar (Figma "Controls", 491:1779) ------------------------------------

def control_bar(L, art, oy, W, *, kind, bg, last):
    """The 54 px bar along the bottom. Positions are those of the Figma component: the left
    group pinned left, the right group pinned right, the seek track stretching between.
    last(x, y, anchor) draws the rightmost button."""
    t = art.t
    fixed = kind == "fsc"
    A = (lambda a: "tl") if fixed else (lambda a: a)
    f16 = L.skin.font(16)
    base = oy + 33 - P.ascent(16)   # the Figma digits sit on y = 33 of the bar

    progress(L, art, x0=254, x1=W - 428, y=oy + 13, h=28, inset=2, r_track=6, r_fill=4,
             handle=(8, 24, 4), anchor_l=A("bl"), anchor_s=A("bottom"), bg=bg)

    L.button(art.icon_button("previous"), 20, oy + 13, "playlist.previous()", "Previous", anchor=A("bl"))
    L.toggle("vlc.isPlaying", art.accent_button("play", 54, 28), art.accent_button("pause", 54, 28),
               "vlc.play()", "vlc.pause()", "Play", "Pause", x=58, y=oy + 13, anchor=A("bl"))
    L.button(art.icon_button("next"), 120, oy + 13, "playlist.next()", "Next", anchor=A("bl"))
    L.text("$T", 170, base, 76, f16, t["text"], align="center", anchor=A("bl"), visible=ACTIVE)

    L.text("$D", W - 420, base, 76, f16, t["text"], align="center", anchor=A("br"), visible=ACTIVE)
    L.toggle("vlc.isMute", art.icon_button("volume"), art.icon_button("mute"), "vlc.mute()", "vlc.mute()",
               "Mute", "Unmute", x=W - 327, y=oy + 13, anchor=A("br"))
    frames = art.get("volume.frames", lambda: P.volume_frames(t), on="chrome")
    L.slider("volume", W - 292, oy + 13, "(0,27),(87,27)", up=art.nothing(), w=88, h=28,
             tip="Volume: $V%", anchor=A("br"), background=(frames, 9, 10), hint="volume")

    tracks_visible = "subtitlewin.isVisible"
    track_buttons = (
        ("audio", "audio tracks", 184, art.icon_button("audio", 30, 32, 3, 4, active=True)),
        ("subtitles", "subtitles", 142, art.icon_button("subtitles_active", 30, 32, 3, 4, original_colors=True)),
    )
    for page, label, offset, active_icon in track_buttons:
        layout = lid(t, page)
        active = f"{layout}.isActive"
        L.button(art.icon_button(page, 30, 32, 3, 4), W - offset, oy + 11,
                 f"subtitlewin.setLayout({layout});subtitlewin.show()", f"Open {label}",
                 anchor=A("br"), visible=f"not {tracks_visible} or not {active}")
        L.button(active_icon, W - offset, oy + 11, "subtitlewin.hide()", f"Close {label}",
                 anchor=A("br"), visible=f"{tracks_visible} and {active}")
    settings = art.icon_button("playback_settings", 30, 32, 3, 4)
    settings_active = art.icon_button("playback_settings_active", 30, 32, 3, 4, active=True)
    select_first = f"preferenceswin.setLayout({lid(t, 'preferences1')})"
    if fixed:
        L.toggle("preferenceswin.isVisible", settings, settings_active, f"{select_first};preferenceswin.show()",
                 "preferenceswin.hide()", "Preferences", "Close preferences", x=W - 100, y=oy + 11, anchor=A("br"))
    elif kind.endswith("_preferences"):
        L.button(settings_active, W - 100, oy + 11, f"main.setLayout({lid(t, kind.removesuffix('_preferences'))})",
                 "Close preferences", anchor=A("br"))
    else:
        L.button(settings, W - 100, oy + 11, f"{select_first};main.setLayout({lid(t, kind + '_preferences')})",
                 "Preferences", anchor=A("br"))
    last(W - 50, oy + 11, A("br"))


# --- the main window --------------------------------------------------------------

def top_bar(L, art, kind):
    """Figma 'topbar' (from Player - Music, 4767:23728): 40 px with the cone, menu, sidebar
    toggle and media button on the left, the title centred, window buttons on the right."""
    t, W = art.t, L.w
    kind = kind.removesuffix("_preferences")
    chrome = art.px("chrome")
    L.image(chrome, 0, 0, W, 40, anchor="top", resize="scale", action="move",
            action2="main.maximize()", visible="not main.isMaximized", hint="titlebar")
    L.image(chrome, 0, 0, W, 40, anchor="top", resize="scale", action="move",
            action2="main.unmaximize()", visible="main.isMaximized", hint="titlebar")

    def cone(bg):
        img = P.blank(38, 28)
        if bg:
            img.alpha_composite(P.rrect(38, 28, (0, 0, 38, 28), 6, t[bg]))
        img.alpha_composite(ICONS.raw("vlc"), (7, 2))
        return img
    L.button(art.states("tb.cone", cone, ((None,), ("hover_bg",), ("down_bg",)), "chrome"),
             6, 6, "dialogs.popup()", "VLC menu")
    L.button(art.icon_button("more", 28, 28, 2, 2), 51, 6, f"{lid(t, kind)}_menu.show()", "Menu")
    if kind == "side":
        L.button(art.icon_button("sidebar", 28, 28, 2, 2, active=True), 89, 6,
                 f"main.setLayout({lid(t, 'full')})", "Hide playlist")
    else:
        L.button(art.icon_button("sidebar", 28, 28, 2, 2), 89, 6, f"main.setLayout({lid(t, 'side')})",
                 "Show playlist")
    L.image(art.get("tb.divider", lambda: ICONS.tint("divider", t["text_dim"]), "chrome"), 117, 8)

    # the Figma breadcrumb shows the media's folder, which skins2 cannot know: it opens media
    label = "Open media"
    L.button(art.labelled_button("tb.crumb", "folder", label, 32 + P.text_width(label, 14) + 10, 28,
                                 2, 2, 32, 19, 14, "chrome"),
             141, 6, "dialogs.fileSimple()", "Open a file")

    f14 = L.skin.font(14)
    y = 25 - P.ascent(14)
    # centred, clear of the left group (ends at 251) and the window buttons (start at W - 150);
    # a title too long to fit scrolls, as VLC skins do, rather than being cut off at both ends
    L.text("$N", 300, y, W - 600, f14, t["text"], align="center", anchor="top", visible=ACTIVE, scrolling="auto")
    L.text("VLC Media Player", 300, y, W - 600, f14, t["text"], align="center", anchor="top", visible=STOPPED)

    L.button(art.window_button("minimize"), W - 150, 8, "vlc.minimize()", "Minimize", anchor="tr")
    L.button(art.window_button("maximize"), W - 114, 8, "main.maximize()", "Maximize", anchor="tr",
             visible="not main.isMaximized")
    L.button(art.window_button("maximize"), W - 114, 8, "main.unmaximize()", "Restore", anchor="tr",
             visible="main.isMaximized")
    L.button(art.window_button("fullscreen"), W - 78, 8, "vlc.fullscreen()", "Fullscreen", anchor="tr")
    L.button(art.window_button("close", danger=True), W - 42, 8, "vlc.quit()", "Quit VLC", anchor="tr")


def stage(L, art, x0, y0, x1, y1, outside):
    """The video area (Figma: the rounded rectangle inset under the top bar). Black whenever
    there is nothing to show, as in VLC. Audio without video shows its cover and title, as in
    Figma 'Player - Music' (whose blurred backdrop needs translucency skins2 lacks); without
    a cover, just the title. outside: palette key of the colour around each rounded corner,
    in tl, tr, br, bl order."""
    t = art.t
    w, h = x1 - x0, y1 - y0
    black = art.px("stage")
    L.image(black, x0, y0, w, h, anchor="fill", resize="scale", action2="dialogs.fileSimple()", hint="stage")
    # the media's cover; with none, its fallback is the black pixel, so nothing shows
    L.image(black, x0 + 60, y0 + 48, w - 120, h - 48 - 120, anchor="fill", resize="scale2", art=True,
            visible=ART_VIEW, hint="cover")
    f20 = L.skin.font(20)
    L.text("$N", x0 + 20, y1 - 62 - P.ascent(20), w - 40, f20, t["stage_text"], align="center", scrolling="auto",
           anchor="bottom", visible=ART_VIEW)

    L.video(x0, y0, w, h, visible="vlc.hasVout")

    # rounded corners: patches of the outside colour laid over the stage's corners
    r = 8
    spots = ((x0, y0, "tl"), (x1 - r, y0, "tr"), (x1 - r, y1 - r, "br"), (x0, y1 - r, "bl"))
    for (cx, cy, which), out in zip(spots, outside):
        img = lambda which=which, out=out: P.corner(r, t[out], which)
        L.image(art.get(f"corner.{which}.{r}.{out}", img, "stage"), cx, cy, anchor=which, hint="corner")


def repeat_controls(L, art, x, y, on):
    """Off -> repeat all -> repeat one -> off, the way VLC's own button cycles."""
    t = art.t
    off = art.icon_button("repeat", 28, 28, 2, 2, on=on)
    all_ = art.icon_button("repeat", 28, 28, 2, 2, active=True, on=on)

    def one_draw(bg, color):
        img = P.blank(28, 28)
        if bg:
            img.alpha_composite(P.rrect(28, 28, (0, 0, 28, 28), 6, t[bg]))
        img.alpha_composite(ICONS.tint("repeat", t[color]), (2, 2))
        return P.text(img, 14, 18, "1", 9, t[color], weight="semibold", align="center")
    one = art.states("repeat1", one_draw, ((None, "accent"), ("hover_bg", "accent_hover"), ("down_bg", "accent_down")), on)
    L.toggle("playlist.isLoop", off, all_, "playlist.setLoop(true)", "playlist.setLoop(false);playlist.setRepeat(true)",
               "Repeat all", "Repeat one", x=x, y=y, visible="not playlist.isRepeat")
    L.button(one, x, y, "playlist.setRepeat(false)", "Repeat off", visible="playlist.isRepeat")


# Playlist rows (Figma sidebar list): ROW px tall, text in the "Inter Rows" font
ROW, ROW_TEXT = 32, 14


def playtree_item(t):
    """Item icon for playlist rows. skins2 makes a row as tall as this image plus 1 and puts
    the text this image's width plus 2 to the right, so the 36 px width lines the text up
    with 'Add media' above it. The icon is centred in the row."""
    icon = ICONS.tint("media", t["icon"])
    bbox = icon.getbbox()
    img = P.blank(36, ROW - 1)
    # skins2 draws the image 1 px below the row top
    img.alpha_composite(icon, (4, round(ROW / 2 - 1 - (bbox[1] + bbox[3]) / 2)))
    return img


def sidebar(L, art):
    """Figma 'sidebar' (848:1996): a 48 px rail and a 379 px panel, here holding the playlist.
    VLC 3 has no media library views, so the rail offers VLC's ways to open media instead."""
    t, H = art.t, L.h
    top, bottom = 40, H - 54
    rail = (("playlist", f"main.setLayout({lid(t, 'full')})", "Hide playlist", True),
            ("file", "dialogs.file()", "Open files", False),
            ("folder", "dialogs.directory()", "Open a folder", False),
            ("disc", "dialogs.disc()", "Open a disc", False),
            ("streaming", "dialogs.net()", "Open a network stream", False),
            ("info", "dialogs.fileInfo()", "Media information", False))
    for i, (icon, action, tip, active) in enumerate(rail):
        L.button(art.icon_button(icon, 36, 36, 6, 6, active=active), 6, 47 + 46 * i, action, tip)
    L.button(art.icon_button("preferences", 36, 36, 6, 6), 6, bottom - 46, "dialogs.prefs()", "Preferences",
             anchor="bl")

    x0, x1 = 48, 427
    L.image(art.px("panel"), x0, top, x1 - x0, bottom - top, anchor="left", resize="scale", hint="sbpanel")
    L.text("Playlist", 70, 79 - P.ascent(16, "semibold"), 200, L.skin.font(16, "semibold"), t["text_hi"])
    L.toggle("playlist.isRandom", art.icon_button("shuffle", 28, 28, 2, 2, on="panel"),
               art.icon_button("shuffle", 28, 28, 2, 2, active=True, on="panel"), "playlist.setRandom(true)",
               "playlist.setRandom(false)", "Shuffle", "Shuffle off", x=301, y=58)
    repeat_controls(L, art, 339, 58, "panel")
    L.button(art.icon_button("open", 28, 28, 2, 2, on="panel"), 377, 58, "dialogs.playlist()",
             "Open VLC's playlist window")

    L.text("Queue", 70, 120 - P.ascent(13), 120, L.skin.font(13), t["text_dim"])
    for i, (icon, action, tip) in enumerate((("sort", "playlist.sort()", "Sort by title"),
                                             ("folder", "playlist.load()", "Load a playlist file"),
                                             ("save", "playlist.save()", "Save the playlist"),
                                             ("remove", "playlist.del()", "Remove selected items"))):
        L.button(art.icon_button(icon, 28, 28, 2, 2, on="panel"), 263 + 38 * i, 100, action, tip)
    L.image(art.px("divider"), 70, 142, 336, 1, resize="scale", hint="sbdivider")

    # 'Add media' and the playlist rows share their height and their icon and text columns
    L.button(art.labelled_button("sb.add", "add", "Add media", 342, ROW, 4, (ROW - 24) // 2, 38,
                                 ROW // 2 + 5, ROW_TEXT, "panel", colors=("text", "icon_hover", "icon_down")),
             64, 152, "playlist.add()", "Add files to the playlist")

    ty, th = 152 + ROW + 4, bottom - 10 - (152 + ROW + 4)
    thumb = art.get("sb.thumb", lambda: P.rrect(4, 40, (0, 0, 4, 40), 2, P.mix(t["panel"], t["text"], .35)), "panel")
    thumb_over = art.get("sb.thumb.over", lambda: P.rrect(4, 40, (0, 0, 4, 40), 2, P.mix(t["panel"], t["text"], .7)), "panel")
    L.playtree(64, ty, 342, th, L.skin.font(ROW_TEXT, "rows"),
               dict(fgcolor=t["row_text"], playcolor=t["accent"], selcolor=t["row_sel"],
                    bgcolor1=t["panel"], bgcolor2=t["panel"]),
               art.get("pt.item", lambda: playtree_item(t), "panel"), anchor="left",
               scrollbar=(410, ty, f"(2,{th - 20}),(2,20)", thumb, thumb_over))


def full_layout(skin, win, t, kind):
    """The resizable main player, with an optional playlist and preferences sidebar."""
    side = kind.removesuffix("_preferences") == "side"
    preferences = kind.endswith("_preferences")
    W, H = 1280, 720
    L = Layout(skin, win, lid(t, kind), W, H, min_size=(1040 if side else 860, 420))     # 860: room for a 174 px seek track
    art = Art(skin, t)
    L.image(art.px("chrome"), 0, 0, W, H, anchor="fill", resize="scale", action="move", hint="bg")
    stage_right = W - PREFERENCES_MARGIN - (PREFERENCES_WIDTH if preferences else 0)
    if side:
        sidebar(L, art)
        stage(L, art, 427, 40, stage_right, H - 54, ("panel", "chrome", "chrome", "panel"))
    else:
        stage(L, art, 10, 40, stage_right, H - 54, ("chrome",) * 4)
    if preferences:
        with L.panel(stage_right, PREFERENCES_TOP, PREFERENCES_WIDTH, PREFERENCES_HEIGHT, anchor="right"):
            preferences_sidebar(L, art, windowed=True)
    top_bar(L, art, kind)

    def last(x, y, anchor):
        if side:
            L.button(art.icon_button("lists", 30, 32, 3, 4, active=True), x, y,
                     f"main.setLayout({lid(t, 'full')})", "Hide playlist", anchor=anchor)
        else:
            L.button(art.icon_button("lists", 30, 32, 3, 4), x, y,
                     f"main.setLayout({lid(t, 'side')})", "Show playlist", anchor=anchor)
    control_bar(L, art, H - 54, W, kind=kind, last=last,
                bg=lambda x0, y0, x1, y1: P.solid(x1 - x0, y1 - y0, t["chrome"]))
    add_skin_commands(L, t, art.px("chrome"))

    # skins2 windows resize only from the right, the bottom and that corner
    chrome = art.px("chrome")
    L.image(chrome, W - 5, 0, 5, H - 12, anchor="right", resize="scale", action="resizeE", hint="gripE")
    L.image(chrome, 0, H - 4, W - 12, 4, anchor="bottom", resize="scale", action="resizeS", hint="gripS")
    L.image(chrome, W - 12, H - 12, 12, 12, anchor="br", resize="scale", action="resizeSE", hint="gripSE")


# --- mini player (Figma "Miniplayer", 970:3090) ---------------------------------------

def mini_layout(skin, win, t):
    W, H = 391, 339
    L = Layout(skin, win, lid(t, "mini"), W, H)
    art = Art(skin, t)
    vx, vy, vw, vh = 8, 8, 375, 211

    bg_img = P.solid(W, H, t["border"])
    bg_img.alpha_composite(P.rrect(W, H, (1, 1, W - 1, H - 1), 0, t["chrome"]))
    bg_img.alpha_composite(P.rrect(W, H, (vx, vy, vx + vw, vy + vh), 6, t["stage"]))
    L.image(art.get("mini.bg", bg_img), 0, 0, action="move", hint="bg")
    L.image(art.px("stage"), vx, vy + 8, vw, vh - 16, resize="scale2", art=True, visible=ART_VIEW, hint="cover")
    L.video(vx, vy, vw, vh, anchor="tl", visible="vlc.hasVout")

    progress(L, art, x0=vx, x1=vx + vw, y=227, h=12, inset=2, r_track=4, r_fill=3, handle=(4, 8, 2),
             anchor_l="tl", anchor_s="tl", bg=lambda x0, y0, x1, y1: bg_img.crop((x0, y0, x1, y1)),
             name="miniseek")

    f14 = L.skin.font(14)
    L.text("$N", 12, 266 - P.ascent(14), 367, f14, t["text_hi"], visible=ACTIVE, scrolling="auto")

    y = 284
    L.button(art.icon_button("previous"), 8, y, "playlist.previous()", "Previous")
    L.toggle("vlc.isPlaying", art.accent_button("play", 46, 28), art.accent_button("pause", 46, 28),
               "vlc.play()", "vlc.pause()", "Play", "Pause", x=40, y=y)
    L.button(art.icon_button("next"), 88, y, "playlist.next()", "Next")
    f12 = L.skin.font(12)
    L.text("$T / $D", 126, y + 19 - P.ascent(12), 126, f12, t["text"], visible=ACTIVE)
    L.toggle("vlc.isMute", art.icon_button("volume"), art.icon_button("mute"), "vlc.mute()", "vlc.mute()",
               "Mute", "Unmute", x=257, y=y)
    L.button(art.icon_button("lists"), 289, y, f"main.setLayout({lid(t, 'side')})", "Full player with playlist")
    L.button(art.icon_button("window"), 321, y, f"main.setLayout({lid(t, 'full')})", "Full player")
    L.button(art.icon_button("close"), 353, y, "vlc.quit()", "Quit VLC")
    add_skin_commands(L, t, art.px("chrome"))
    return L


# --- media tracks ------------------------------------------------------------------

def tracks_layout(skin, win, t, page):
    # The Windows module uses the layout sizes to attach the matching controls.
    W, H = 368, {"video": 481, "audio": 504, "subtitles": 480}[page]
    L = Layout(skin, win, lid(t, page), W, H)
    art = Art(skin, t)
    body_top = 46
    tab_w = 104
    tabs = {
        "video": (20, 14, 42, "video_active"),
        "audio": (136, 23, 51, None),
        "subtitles": (252, 10, 38, "subtitles_active"),
    }

    bg = P.rrect(W, H, (0, 0, W, H), 12, t["panel"])
    bg.paste(P.solid(W, body_top, t["track"]), (0, 0))
    bg.paste(P.solid(tab_w, 2, t["accent"]), (tabs[page][0], body_top - 2))
    P.text(bg, 26, 80, "Embedded Tracks", 14, t["text"])
    if page == "audio":
        divider_y = 228
        bg.paste(P.solid(W, 1, t["divider"]), (0, divider_y))
        P.text(bg, 26, divider_y + 34, "Output Device", 14, t["text"])
    # Trim the faint resampling fringe from the VLC window outline.
    bg.putalpha(bg.getchannel("A").point(lambda coverage: coverage if coverage >= 128 else 0))
    L.image(art.get(f"{page}.bg", bg, on="track"), 0, 0, hint="bg")

    for tab_page, (tab_x, icon_x, text_x, active_icon) in tabs.items():
        active = tab_page == page
        label = tab_page.capitalize()
        use_active_icon = active and active_icon is not None
        icon = active_icon if use_active_icon else tab_page
        tab = art.labelled_button(f"{tab_page}.tab.{page}", icon, label, tab_w, 32,
                                  icon_x, 4, text_x, 21, 13, "track",
                                  icon_color="accent" if active and tab_page == "audio" else None,
                                  original_colors=use_active_icon)
        action = "subtitlewin.hide()" if active else f"subtitlewin.setLayout({lid(t, tab_page)})"
        L.button(tab, tab_x, 7, action, "Close panel" if active else f"Show {label}")

    prefs_w, prefs_h = W - 36, 32

    def draw_prefs(background, color):
        img = P.rrect(prefs_w, prefs_h, (0, 0, prefs_w, prefs_h), 4, t[background])
        return P.text(img, prefs_w // 2, 21, "Show Preferences", 14, t[color], align="center")

    prefs = art.states("subtitles.prefs", draw_prefs,
                       (("track", "text"), ("hover_bg", "text_hi"), ("down_bg", "text_hi")), "panel")
    L.button(prefs, 18, H - 18 - prefs_h, "subtitlewin.hide();dialogs.prefs()", "Show Preferences")
    return L


# --- preferences -------------------------------------------------------------------

def preferences_sidebar(L, art, *, windowed):
    """Draw the same right-side panel in the player and over fullscreen video."""
    t = art.t
    W, H = PREFERENCES_WIDTH, PREFERENCES_HEIGHT
    frame = "stage" if t is DARK else "card_outer"
    left, right, bottom = PREFERENCES_BODY_LEFT, PREFERENCES_BODY_RIGHT, H - PREFERENCES_BODY_BOTTOM
    body_width = W - left - right
    tab_width = body_width // PREFERENCES_TABS
    tab_height, baseline, font_size = PREFERENCES_HEADER - 14, 37, 13
    tab_colors = ("text", "text_hi", "text")
    radius = PREFERENCES_CORNER

    L.image(art.px(frame), 0, 0, W, H, anchor="left", resize="scale", hint="preferencesFrame")
    L.image(art.px("panel"), left, PREFERENCES_HEADER, body_width, bottom - radius - PREFERENCES_HEADER,
            anchor="left", resize="scale", hint="preferencesBody")
    L.image(art.get("preferences.bodyEnd", lambda: P.rrect(body_width, 2 * radius,
                (0, 0, body_width, 2 * radius), radius, t["panel"]).crop((0, radius, body_width, 2 * radius)),
                frame), left, bottom - radius, anchor="bl")
    L.image(art.px("divider"), left, PREFERENCES_HEADER, body_width, 1, resize="scale")

    def labelled_tab(label, icon, *, highlighted=False):
        icon_slot = 24 + 8
        icon_x = (tab_width - icon_slot - P.text_width(label, font_size)) // 2
        return art.labelled_button(f"preferences.tab.{icon}", icon, label, tab_width, tab_height,
                                   icon_x, 20, icon_x + icon_slot, baseline, font_size, frame,
                                   colors=tab_colors, original_colors=highlighted)

    labelled_tabs = (
        ("Subtitles", "subtitles", "subtitles_outline_active"),
        ("Audio", "audio", "audio_two_tone"),
        ("Video", "video", "video_active"),
        ("Playback", "playback_tab", "playback_tab_active"),
    )
    for number, (label, icon, active_icon) in enumerate(labelled_tabs, 1):
        x = left + (PREFERENCES_TABS - number) * tab_width
        page = lid(t, f"preferences{number}")
        action, selected = f"preferenceswin.setLayout({page})", f"{page}.isActive"
        L.toggle(selected, labelled_tab(label, icon), labelled_tab(label, active_icon, highlighted=True),
                 action, action, label, label, x=x, y=PREFERENCES_TAB_TOP, hint="preferencesTab")
        L.image(art.px("accent"), x + 4, PREFERENCES_HEADER - 2, tab_width - 8, 2, resize="scale",
                visible=selected, hint="preferencesUnderline")

    if windowed:
        for corner, y in (("tr", 0), ("br", H - 8)):
            mask = art.get(f"corner.{corner}.8.chrome", lambda c=corner: P.corner(8, t["chrome"], c), frame)
            L.image(mask, W - 8, y, anchor=corner, hint="corner")


def preferences_layout(skin, win, t, number):
    W, H = PREFERENCES_WIDTH, PREFERENCES_HEIGHT
    L = Layout(skin, win, lid(t, f"preferences{number}"), W, H, min_size=(W, 220), max_size=(W, 99999))
    art = Art(skin, t)
    preferences_sidebar(L, art, windowed=False)
    add_skin_commands(L, t, art.px("stage"))
    # The Windows module uses the corner to resize the panel to the fullscreen stage.
    L.image(art.px("stage" if t is DARK else "card_outer"), W - 12, H - 12, 12, 12,
            anchor="br", resize="scale", action="resizeSE", hint="gripSE")


# --- fullscreen controller ----------------------------------------------------------

def fsc_layout(skin, win, t):
    """The bar again, floating over fullscreen video."""
    W, H = 1000, 56
    L = Layout(skin, win, lid(t, "fsc"), W, H)
    art = Art(skin, t)
    bg_img = P.solid(W, H, t["border"])
    bg_img.alpha_composite(P.rrect(W, H, (1, 1, W - 1, H - 1), 0, t["chrome"]))
    L.image(art.get("fsc.bg", bg_img), 0, 0, action="move", hint="bg")

    def last(x, y, anchor):
        L.button(art.icon_button("fullscreen_exit", 30, 32, 3, 4), x, y, "vlc.fullscreen()", "Leave fullscreen",
                 anchor=anchor)
    control_bar(L, art, 1, W, kind="fsc", last=last, bg=lambda x0, y0, x1, y1: bg_img.crop((x0, y0, x1, y1)))
    add_skin_commands(L, t, art.px("chrome"))
    return L


# --- menus ----------------------------------------------------------------------

def menus(skin):
    """The '...' menu (Figma 'menu', 6062:13354) per player layout."""
    for t in THEMES:
        o = other(t)
        for kind in ("full", "side", "mini"):
            view = [("Hide Playlist" if kind == "side" else "Show Playlist",
                     f"main.setLayout({lid(t, 'full' if kind == 'side' else 'side')})"),
                    ("Full Player" if kind == "mini" else "Mini Player",
                     f"main.setLayout({lid(t, 'full' if kind == 'mini' else 'mini')})"),
                    (f"{o['name'].title()} Theme", switch_theme(t, kind)),
                    ("Always on Top", "vlc.onTop()")]
            skin.menu(f"{lid(t, kind)}_menu", [
                ("Open File(s)...", "dialogs.file()"),
                ("Open Folder...", "dialogs.directory()"),
                ("Open Network Stream...", "dialogs.net()"),
                ("Open Disc...", "dialogs.disc()"),
                ("Convert / Stream...", "dialogs.streamingWizard()"),
                None,
                *view,
                None,
                ("Media Information", "dialogs.fileInfo()"),
                ("Messages", "dialogs.messages()"),
                ("Change Skin...", "dialogs.changeSkin()"),
                ("Settings...", "dialogs.prefs()"),
                None,
                ("Quit VLC\tCtrl+Q", "vlc.quit()"),
            ])


# --- build -------------------------------------------------------------------------

MAIN_KINDS = ("full", "side", "mini")


def build(start="dark_full", preview_fsc=False, *, dist_dir=None):
    """Use `start` until VLC has a saved layout. `preview_fsc` opens the fullscreen
    controller as an ordinary window for screenshots.
    dist_dir lets the Windows test player build the same skin in its own folder."""
    dist = ROOT / "dist" if dist_dir is None else Path(dist_dir)
    output = dist / NAME
    if output.exists():
        shutil.rmtree(output)
    rows = P.rows_font(Path(tempfile.mkdtemp()) / "InterRows-Regular.ttf", ROW_TEXT, ROW)
    skin = Skin("VLC Redesign", "Design by Juxtopposed, skinned by build.py", output,
                {**P.FONT_FILES, "rows": rows}, P.FONT_LICENSE,
                webpage="https://github.com/mamogh/vlc-redesign-skin")
    menus(skin)

    # skins2 starts each window on the layout added last
    start_t = DARK if start.startswith("dark") else LIGHT
    main = skin.window("main", position="Center", dragdrop="true", playondrop="true")
    kinds = (*MAIN_KINDS, "full_preferences", "side_preferences")
    order = [(t, k) for t in THEMES for k in kinds if lid(t, k) != start] + [(start_t, start.split("_")[1])]
    for t, kind in order:
        if kind == "mini":
            mini_layout(skin, main, t)
        else:
            full_layout(skin, main, t, kind)
    subtitles = skin.window("subtitlewin", visible="false", position="Center")
    # The hidden window's active layout also holds the selected tab in the windowed player.
    preferences = skin.window("preferenceswin", visible="false", position="NorthEast", xmargin=10, ymargin=40)
    fsc = (skin.window("fscpreview", position="Center") if preview_fsc else
           skin.window("fullscreenController", position="South", ymargin="3%"))
    for t in (other(start_t), start_t):
        tracks_layout(skin, subtitles, t, "video")
        tracks_layout(skin, subtitles, t, "audio")
        tracks_layout(skin, subtitles, t, "subtitles")
        for number in range(1, PREFERENCES_TABS + 1):
            preferences_layout(skin, preferences, t, number)
        fsc_layout(skin, fsc, t)

    skin.write()
    problems = check_theme.check(output / "theme.xml")
    if problems:
        raise SystemExit("theme.xml has problems:\n  " + "\n  ".join(problems))
    vlt = dist / f"{NAME}.vlt"
    # dist/ is committed, so the archive is made byte-for-byte reproducible: no timestamps,
    # owners or ordering that change from build to build
    def plain(info):
        info.mtime, info.uid, info.gid, info.uname, info.gname = 0, 0, 0, "", ""
        return info
    with open(vlt, "wb") as raw, gzip.GzipFile(fileobj=raw, mode="wb", mtime=0, filename="") as gz, \
            tarfile.open(fileobj=gz, mode="w", format=tarfile.USTAR_FORMAT) as tar:
        tar.add(output, arcname=NAME, recursive=False, filter=plain)
        for path in sorted(output.rglob("*")):
            tar.add(path, arcname=f"{NAME}/{path.relative_to(output).as_posix()}", recursive=False, filter=plain)
    images = len(list((output / "img").glob("*.png")))
    print(f"{output / 'theme.xml'}: {images} images")
    print(f"{vlt}: {vlt.stat().st_size // 1024} KiB")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--start", default="dark_full", choices=[lid(t, k) for t in THEMES for k in MAIN_KINDS],
                    help="layout the main window opens with")
    ap.add_argument("--preview-fsc", action="store_true",
                    help="show the fullscreen controller as a normal window (for screenshots)")
    a = ap.parse_args()
    build(a.start, preview_fsc=a.preview_fsc)
