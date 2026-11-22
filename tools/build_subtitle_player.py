"""Build a portable VLC 3.0.24 test player with live Video, Audio and Subtitles tabs."""
import argparse
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from build import NAME, build  # noqa: E402

MODULE = "libsubtitlepanel_plugin.dll"
CORE_EXPORTS = ("playlist_CurrentInput", "vlc_object_release", "var_Get", "var_Set",
                "var_Change", "var_FreeList", "input_GetItem", "input_item_GetURI",
                "playlist_GetAout", "aout_DevicesList", "aout_DeviceGet", "aout_DeviceSet", "vlc_gettext")
LIBVLC_EXPORTS = ("libvlc_new", "libvlc_release", "libvlc_free", "libvlc_media_new_location",
                  "libvlc_media_release", "libvlc_media_parse_with_options",
                  "libvlc_media_get_parsed_status", "libvlc_media_parse_stop",
                  "libvlc_media_tracks_get", "libvlc_media_tracks_release")


def build_module(output, toolchain):
    with TemporaryDirectory() as temporary:
        temp = Path(temporary)
        import_libraries = []
        for lib, exports in (("libvlccore", CORE_EXPORTS), ("libvlc", LIBVLC_EXPORTS)):
            definition = temp / f"{lib}.def"
            definition.write_text(f"LIBRARY {lib}.dll\nEXPORTS\n" + "\n".join(exports), encoding="ascii")
            import_library = temp / f"{lib}.dll.a"
            subprocess.run([toolchain / "dlltool.exe", "--machine", "i386:x86-64", "--input-def", definition,
                            "--output-lib", import_library], check=True)
            import_libraries.append(import_library)
        subprocess.run([toolchain / "g++.exe", "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-shared", "-static", "-o", output, ROOT / "tools/subtitle_panel.cpp",
                        *import_libraries, "-lgdi32", "-luser32", "-lcomctl32"], check=True)


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
