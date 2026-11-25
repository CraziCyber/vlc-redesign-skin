"""Build a portable VLC 3.0.24 test player with live Video, Audio and Subtitles tabs."""
import argparse
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from build import (  # noqa: E402
    NAME, PREFERENCES_BODY_BOTTOM, PREFERENCES_BODY_LEFT, PREFERENCES_BODY_RIGHT, PREFERENCES_BOTTOM,
    PREFERENCES_CORNER, PREFERENCES_HEADER, PREFERENCES_MARGIN, PREFERENCES_TABS, PREFERENCES_TAB_TOP,
    PREFERENCES_TOP, PREFERENCES_WIDTH, build,
)
from skinlib.context_menu import write_resources  # noqa: E402
from skinlib.paint import FIGMA  # noqa: E402

MODULE = "libsubtitlepanel_plugin.dll"
CORE_EXPORTS = ("playlist_CurrentInput", "playlist_Lock", "playlist_Unlock", "vlc_object_release",
                "var_Create", "var_Destroy", "var_Get", "var_Set",
                "var_Change", "var_FreeList", "var_Inherit", "var_Type", "input_GetItem", "input_item_GetURI", "input_item_GetTitleFbName",
                "input_item_GetMeta", "vlc_uri2path",
                "input_Control", "vlc_path2uri",
                "playlist_GetAout", "playlist_EnableAudioFilter", "aout_DevicesList", "aout_DeviceGet", "aout_DeviceSet", "vlc_gettext",
                "playlist_Status", "playlist_SetRenderer", "config_GetPsz", "config_PutPsz", "config_GetPszChoices",
                "config_ChainCreate", "config_ChainDestroy", "config_StringEscape",
                "es_format_Copy", "es_format_Clean", "es_format_IsSimilar", "vlc_fourcc_GetChromaDescription",
                "vlc_renderer_item_hold", "vlc_renderer_item_release",
                "vlc_renderer_item_name", "vlc_renderer_item_sout", "vlc_renderer_item_flags",
                "config_GetInt", "config_PutInt", "config_GetFloat", "config_PutFloat",
                "config_SaveConfigFile", "demux_vaControl", "module_get_object", "text_style_Delete",
                "vlc_list_children", "vlc_list_release", "GetLang_1",
                "GetLang_2T", "GetLang_2B",
                "vlc_actions_get_id", "vlc_actions_get_key_names", "vlc_actions_get_keycodes",
                "vlc_keycode2str", "vlc_str2keycode")
LIBVLC_EXPORTS = ("libvlc_new", "libvlc_release", "libvlc_free", "libvlc_media_new_location",
                  "libvlc_media_release", "libvlc_media_parse_with_options",
                  "libvlc_media_get_parsed_status", "libvlc_media_parse_stop",
                  "libvlc_media_tracks_get", "libvlc_media_tracks_release", "libvlc_media_new_from_input_item",
                  "libvlc_audio_equalizer_new_from_preset", "libvlc_audio_equalizer_release",
                  "libvlc_audio_equalizer_get_preamp", "libvlc_audio_equalizer_get_band_count",
                  "libvlc_audio_equalizer_get_amp_at_index", "libvlc_media_new_callbacks",
                  "libvlc_media_player_new_from_media", "libvlc_media_player_play", "libvlc_media_player_stop",
                  "libvlc_media_player_get_state", "libvlc_media_player_release", "libvlc_audio_output_device_set",
                  "libvlc_renderer_discoverer_list_get", "libvlc_renderer_discoverer_list_release",
                  "libvlc_renderer_discoverer_new", "libvlc_renderer_discoverer_release",
                  "libvlc_renderer_discoverer_event_manager", "libvlc_renderer_discoverer_start",
                  "libvlc_event_attach")

# VLC preset keys and the corresponding 24 px symbols in the Figma icon sheet.
PRESET_ICONS = {
    "flat": (372, 592), "classical": (416, 592), "club": (460, 592), "dance": (504, 592),
    "fullbass": (20, 636), "fullbasstreble": (64, 636), "fulltreble": (108, 636), "headphones": (152, 636),
    "largehall": (416, 636), "live": (64, 680), "party": (20, 680), "pop": (504, 636),
    "reggae": (460, 636), "rock": (196, 636), "ska": (240, 636), "soft": (284, 636),
    "softrock": (372, 636), "techno": (328, 636),
}


def write_icon_strip(bitmap, size, cells):
    sheet = Image.open(FIGMA / "icons-42_356.png").convert("RGBA")
    atlas = Image.new("RGBA", (len(cells) * size, size))
    for index, (x, y) in enumerate(cells):
        atlas.paste(sheet.crop((x, y, x + size, y + size)), (index * size, 0))
    bitmap.write_bytes(atlas.convert("RGBa").tobytes("raw", "BGRa"))


def write_preset_icons(directory):
    size, resource = 24, 104
    # The last cell is the orange selection check.
    cells = [*PRESET_ICONS.values(), (240, 416)]
    bitmap = directory / "equalizer_icons.bgra"
    write_icon_strip(bitmap, size, cells)
    keys = ", ".join(f'"{key}"' for key in PRESET_ICONS)
    (directory / "equalizer_icons.hpp").write_text(
        "#pragma once\n\nnamespace equalizer_icons {\n"
        f"constexpr int resource = {resource}, size = {size}, count = {len(cells)};\n"
        f"constexpr const char *keys[] = {{ {keys} }};\n"
        "}\n", encoding="ascii",
    )
    return f'{resource} RCDATA "{bitmap.as_posix()}"\n'


def write_crop_link_icons(directory):
    size, resource = 24, 105
    cells = [(108, 416), (152, 416)]
    bitmap = directory / "crop_link_icons.bgra"
    write_icon_strip(bitmap, size, cells)
    (directory / "crop_link_icons.hpp").write_text(
        "#pragma once\n\nnamespace crop_link_icons {\n"
        f"constexpr int resource = {resource}, size = {size}, count = {len(cells)};\n"
        "}\n", encoding="ascii",
    )
    return f'{resource} RCDATA "{bitmap.as_posix()}"\n'


def build_module(output, toolchain):
    with TemporaryDirectory() as temporary:
        temp = Path(temporary)
        geometry = dict(width=PREFERENCES_WIDTH, top=PREFERENCES_TOP, bottom=PREFERENCES_BOTTOM,
                        margin=PREFERENCES_MARGIN, header=PREFERENCES_HEADER, tab_top=PREFERENCES_TAB_TOP,
                        tabs=PREFERENCES_TABS, body_left=PREFERENCES_BODY_LEFT, body_right=PREFERENCES_BODY_RIGHT,
                        body_bottom=PREFERENCES_BODY_BOTTOM, corner=PREFERENCES_CORNER)
        (temp / "preferences_layout.hpp").write_text(
            "#pragma once\n\nnamespace preferences_layout {\n"
            + "".join(f"constexpr int {name} = {value};\n" for name, value in geometry.items()) + "}\n",
            encoding="ascii",
        )
        resources = temp / "context_menu.o"
        resource_script = write_resources(temp)
        with resource_script.open("a", encoding="utf-8") as script:
            script.write(write_preset_icons(temp))
            script.write(write_crop_link_icons(temp))
            dialog_resource = ROOT / "tools/add_subtitles.rc"
            script.write(f'#include "{dialog_resource.as_posix()}"\n')
        subprocess.run([toolchain / "windres.exe", "-c", "65001", "-i", resource_script,
                        "-o", resources, "-O", "coff"], check=True)
        import_libraries = []
        for lib, exports in (("libvlccore", CORE_EXPORTS), ("libvlc", LIBVLC_EXPORTS)):
            definition = temp / f"{lib}.def"
            definition.write_text(f"LIBRARY {lib}.dll\nEXPORTS\n" + "\n".join(exports), encoding="ascii")
            import_library = temp / f"{lib}.dll.a"
            subprocess.run([toolchain / "dlltool.exe", "--machine", "i386:x86-64", "--input-def", definition,
                            "--output-lib", import_library], check=True)
            import_libraries.append(import_library)
        subprocess.run([toolchain / "g++.exe", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-shared", "-static", "-I", temp, "-o", output, ROOT / "tools/subtitle_panel.cpp",
                        ROOT / "tools/add_subtitles.cpp", ROOT / "tools/context_menu.cpp", ROOT / "tools/hidden_controls.cpp",
                        ROOT / "tools/player_window.cpp", ROOT / "tools/preferences_sidebar.cpp",
                        ROOT / "tools/subtitle_preferences.cpp", ROOT / "tools/subtitle_preferences_view.cpp",
                        ROOT / "tools/track_defaults.cpp", ROOT / "tools/audio_preferences.cpp",
                        ROOT / "tools/audio_preferences_view.cpp", ROOT / "tools/stereo_test.cpp",
                        ROOT / "tools/video_preferences.cpp", ROOT / "tools/video_preferences_view.cpp",
                        ROOT / "tools/video_renderers.cpp",
                        ROOT / "tools/subtitle_style_filter.cpp", ROOT / "tools/subtitle_speed_filter.cpp",
                        ROOT / "tools/opensubtitles.cpp", resources,
                        ROOT / "tools/subtitle_folder.cpp", ROOT / "tools/local_subtitles.cpp", ROOT / "tools/subtitle_drop.cpp",
                        *import_libraries, "-lgdiplus", "-lgdi32", "-lmsimg32", "-luser32", "-lcomctl32", "-lcomdlg32", "-ldwmapi",
                        "-lwinhttp", "-lruntimeobject", "-ladvapi32", "-lole32", "-lshell32", "-luuid"], check=True)


def prepare_player(vlc, output, toolchain):
    with os.add_dll_directory(str(vlc)):
        library = ctypes.CDLL(str(vlc / "libvlc.dll"))
        library.libvlc_get_version.restype = ctypes.c_char_p
        version = library.libvlc_get_version().decode("ascii").split()[0]
    if version != "3.0.24" or ctypes.sizeof(ctypes.c_void_p) != 8:
        raise SystemExit("The subtitle test player requires VLC 3.0.24 and a 64-bit Python on Windows.")
    if output == vlc or vlc in output.parents or output in vlc.parents:
        raise SystemExit("Choose a separate destination for the VLC test player.")

    output.mkdir(parents=True, exist_ok=True)
    for directory in ("plugins", "locale", "lua", "skins", "hrtfs"):
        source = vlc / directory
        if source.is_dir():
            shutil.copytree(source, output / directory, dirs_exist_ok=True)
    for filename in ("vlc.exe", "libvlc.dll", "libvlccore.dll", "COPYING.txt", "AUTHORS.txt"):
        shutil.copy2(vlc / filename, output / filename)
    (output / "portable").mkdir(exist_ok=True)
    build_module(output / "plugins/gui" / MODULE, toolchain)
    build(dist_dir=output / "skins")

    launcher = output / "Start-VLC-Subtitle-Test.cmd"
    launcher.write_text(
        '@echo off\n'
        '"%~dp0vlc.exe" --no-one-instance -I skins --extraintf=subtitlepanel '
        '--demux-filter=redesignsubtitlespeed '
        f'--skins2-last="%~dp0skins\\{NAME}.vlt" %*\n', encoding="ascii"
    )
    print(launcher)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--vlc", type=Path, default=Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "VideoLAN/VLC")
    parser.add_argument("--output", type=Path, default=ROOT / "testplayer")
    parser.add_argument("--toolchain", type=Path, default=Path("C:/msys64/mingw64/bin"))
    args = parser.parse_args()
    prepare_player(args.vlc.resolve(), args.output.resolve(), args.toolchain.resolve())
