#!/usr/bin/env python3
"""Compile the production ESP bridge against fake UARTs and the AT32 codec.

Requires native C/C++ compilers, or --zig /path/to/zig.exe. All output is built
in a temporary directory; no connected device is opened or flashed.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zig", help="Zig executable to use for both C and C++")
    parser.add_argument("--msvc", action="store_true", help="Use cl.exe from a VS Developer Prompt")
    args = parser.parse_args()
    tests = Path(__file__).resolve().parent
    root = tests.parents[1]
    drivers = root / "firmware/src/drivers"
    esp_src = tests.parent / "src"
    cc = [args.zig, "cc"] if args.zig else [shutil.which("clang") or shutil.which("gcc") or "cc"]
    cxx = [args.zig, "c++"] if args.zig else [shutil.which("clang++") or shutil.which("g++") or "c++"]
    common = ["-O1", "-Wall", "-Wextra", "-Werror", "-I", str(drivers)]
    with tempfile.TemporaryDirectory(prefix="openscope-bridge-tests-") as directory:
        build = Path(directory)
        if args.msvc:
            common = ["cl", "/nologo", "/W3", "/WX", "/wd4244", "/wd4267", "/wd4018",
                      "/D_CRT_SECURE_NO_WARNINGS", "/I" + str(drivers)]
            codec = build / "codec.obj"
            subprocess.run(common + ["/std:c11", "/c", str(drivers / "esp_link_protocol.c"), "/Fo" + str(codec)], cwd=build, check=True)
            protocol_test = build / "protocol.exe"
            subprocess.run(common + ["/std:c11", str(root / "firmware/tests/test_esp_link_protocol.c"), str(codec), "/Fe" + str(protocol_test)], cwd=build, check=True)
            subprocess.run([str(protocol_test)], check=True)
            bridge_test = build / "bridge.exe"
            subprocess.run(common + ["/std:c++17", "/EHsc", "/I" + str(tests / "stubs"),
                str(tests / "test_meter_bridge.cpp"), str(esp_src / "meter_bridge.cpp"),
                str(esp_src / "link.cpp"), str(esp_src / "app_runtime.cpp"),
                str(esp_src / "measurement_service.cpp"), str(esp_src / "wifi_manager.cpp"),
                str(esp_src / "apps/wifi_settings_app.cpp"), str(codec), "/Fe" + str(bridge_test)], cwd=build, check=True)
            subprocess.run([str(bridge_test)], check=True)
            editor_test = build / "wifi_editor.exe"
            subprocess.run(common + ["/std:c++17", "/EHsc", str(tests / "test_wifi_editor.cpp"),
                "/Fe" + str(editor_test)], cwd=build, check=True)
            subprocess.run([str(editor_test)], check=True)
            wifi_test = build / "wifi_manager.exe"
            subprocess.run(common + ["/std:c++17", "/EHsc", "/I" + str(tests / "stubs"),
                str(tests / "test_wifi_manager.cpp"), str(esp_src / "wifi_manager.cpp"),
                "/Fe" + str(wifi_test)], cwd=build, check=True)
            subprocess.run([str(wifi_test)], check=True)
            telemetry_test = build / "telemetry.exe"
            subprocess.run(common + ["/std:c++17", "/EHsc", "/I" + str(tests / "stubs"),
                str(tests / "test_measurement_service.cpp"), str(esp_src / "measurement_service.cpp"),
                str(esp_src / "instrument_web.cpp"), "/Fe" + str(telemetry_test)], cwd=build, check=True)
            subprocess.run([str(telemetry_test)], check=True)
            for name in ("test_fpga_meter_plan", "test_meter_word_map"):
                target = build / (name + ".exe")
                subprocess.run(common + ["/std:c11", str(root / ("firmware/tests/" + name + ".c")),
                    str(drivers / "fpga_meter_plan.c"), "/Fe" + str(target)], cwd=build, check=True)
                subprocess.run([str(target)], check=True)
            return
        codec = build / "codec.o"
        subprocess.run(cc + ["-std=c11"] + common + ["-c", str(drivers / "esp_link_protocol.c"), "-o", str(codec)], check=True)
        protocol_test = build / "protocol.exe"
        subprocess.run(cc + ["-std=c11"] + common + [str(root / "firmware/tests/test_esp_link_protocol.c"), str(codec), "-o", str(protocol_test)], check=True)
        subprocess.run([str(protocol_test)], check=True)
        bridge_test = build / "bridge.exe"
        subprocess.run(cxx + ["-std=c++17"] + common + ["-I", str(tests / "stubs"),
            str(tests / "test_meter_bridge.cpp"), str(esp_src / "meter_bridge.cpp"),
            str(esp_src / "link.cpp"), str(esp_src / "app_runtime.cpp"),
            str(esp_src / "measurement_service.cpp"), str(esp_src / "wifi_manager.cpp"),
            str(esp_src / "apps/wifi_settings_app.cpp"), str(codec), "-o", str(bridge_test)], check=True)
        subprocess.run([str(bridge_test)], check=True)
        editor_test = build / "wifi_editor.exe"
        subprocess.run(cxx + ["-std=c++17"] + common + [str(tests / "test_wifi_editor.cpp"),
            "-o", str(editor_test)], check=True)
        subprocess.run([str(editor_test)], check=True)
        wifi_test = build / "wifi_manager.exe"
        subprocess.run(cxx + ["-std=c++17"] + common + ["-I", str(tests / "stubs"),
            str(tests / "test_wifi_manager.cpp"), str(esp_src / "wifi_manager.cpp"),
            "-o", str(wifi_test)], check=True)
        subprocess.run([str(wifi_test)], check=True)
        telemetry_test = build / "telemetry.exe"
        subprocess.run(cxx + ["-std=c++17"] + common + ["-I", str(tests / "stubs"),
            str(tests / "test_measurement_service.cpp"), str(esp_src / "measurement_service.cpp"),
            str(esp_src / "instrument_web.cpp"), "-o", str(telemetry_test)], check=True)
        subprocess.run([str(telemetry_test)], check=True)
        for name in ("test_fpga_meter_plan", "test_meter_word_map"):
            target = build / (name + ".exe")
            subprocess.run(cc + ["-std=c11"] + common + [str(root / ("firmware/tests/" + name + ".c")),
                str(drivers / "fpga_meter_plan.c"), "-o", str(target)], check=True)
            subprocess.run([str(target)], check=True)


if __name__ == "__main__":
    main()
