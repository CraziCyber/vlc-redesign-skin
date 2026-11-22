"""Skin actions and artwork shared by the skin and the Windows context menu."""

from pathlib import Path

from PIL import Image

from .paint import FIGMA, FONT_FILES, FONT_LICENSE


# A positive client coordinate outside the player, within Win32's signed 16-bit mouse range.
COMMAND_X = 30000
SKIN_ACTIONS = {
    "open_files": "dialogs.file()",
    "open_folder": "dialogs.directory()",
    "open_disc": "dialogs.disc()",
    "open_network": "dialogs.net()",
    "convert": "dialogs.streamingWizard()",
    "settings": "dialogs.prefs()",
    "media_info": "dialogs.fileInfo()",
    "messages": "dialogs.messages()",
    "change_skin": "dialogs.changeSkin()",
    "vlc_menu": "dialogs.popup()",
    "on_top": "vlc.onTop()",
    "eq": "eqwin.show()",
    "video_tracks": "subtitlewin.setLayout({theme}_video);subtitlewin.show()",
    "audio_tracks": "subtitlewin.setLayout({theme}_audio);subtitlewin.show()",
    "subtitles": "subtitlewin.setLayout({theme}_subtitles);subtitlewin.show()",
    "toggle_playlist": "main.setLayout({theme}_{playlist_layout})",
    "load_playlist": "playlist.load()",
    "save_playlist": "playlist.save()",
}

# Coordinates in the original 24 px Figma icon sheet.
ICONS = {
    "play": (20, 108),
    "pause": (108, 108),
    "stop": (284, 108),
    "previous": (372, 108),
    "next": (196, 108),
    "record": (196, 548),
    "hide_ui": (20, 768),
    "playback": (504, 64),
    "video": (372, 240),
    "audio": (328, 64),
    "subtitle": (196, 64),
    "settings": (64, 20),
    "playlist": (372, 20),
    "open_media": (64, 504),
    "quit": (504, 724),
    "chevron": (284, 372),
}


def add_skin_commands(layout, theme, pixel):
    # The module invokes these native skin actions on the Skins2 UI thread.
    # The panel keeps the action targets outside the drawable player area.
    playlist_layout = "full" if layout.id.endswith("_side") else "side"
    with layout.panel(COMMAND_X, 0, 1, len(SKIN_ACTIONS), anchor="tl"):
        for index, (name, action) in enumerate(SKIN_ACTIONS.items()):
            context_action = action.format(theme=theme["name"], playlist_layout=playlist_layout)
            layout.button((pixel, pixel, pixel), 0, index, context_action, hint=name)


def write_resources(directory):
    """Write the generated header and Win32 resources used by the module build."""
    directory = Path(directory)
    icon_size = 24
    sheet = Image.open(FIGMA / "icons-42_356.png").convert("RGBA")
    atlas = Image.new("RGBA", (len(ICONS) * icon_size, icon_size))
    for index, (x, y) in enumerate(ICONS.values()):
        atlas.paste(sheet.crop((x, y, x + icon_size, y + icon_size)), (index * icon_size, 0))
    icon_file = directory / "context_icons.bgra"
    icon_file.write_bytes(atlas.convert("RGBa").tobytes("raw", "BGRa"))
    font = FONT_FILES["regular"].resolve().as_posix()
    rc = directory / "context_menu.rc"
    rc.write_text(f'101 RCDATA "{icon_file.as_posix()}"\n102 RCDATA "{font}"\n'
                  f'103 RCDATA "{FONT_LICENSE.resolve().as_posix()}"\n', encoding="utf-8")
    (directory / "context_menu_assets.hpp").write_text(
        "#pragma once\n\nnamespace context_menu {\n"
        f"enum class SkinCommand {{ {', '.join(SKIN_ACTIONS)}, none = -1, toggle_ui = -2 }};\n"
        f"enum class Icon {{ {', '.join(ICONS)}, none = -1 }};\n"
        f"constexpr int command_x = {COMMAND_X};\n"
        f"constexpr int icon_size = {icon_size};\n"
        f"constexpr int icon_count = {len(ICONS)};\n"
        "}\n", encoding="ascii",
    )
    return rc
