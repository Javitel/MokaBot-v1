# PlatformIO extra script (pre): OTA firmware images per board.
#
# Each environment names its OTA image with `custom_ota_firmware` in
# platformio.ini, and `custom_firmware_version` sets the release version.
# This script:
#  - passes them to the code as OTA_FIRMWARE_FILE and FIRMWARE_VERSION, so
#    the device downloads <OTA folder URL>/<name> and knows its version;
#  - after every build, copies firmware.bin to build/<name> and writes the
#    version to build/<name without .bin>.version, which the device reads to
#    tell whether the published image is newer;
#  - for the web flasher (flash.html), copies the other images a new board
#    needs (bootloader, partition table, boot_app0) to build/<name without
#    .bin>/ and adds the board to build/manifest.json (ESP Web Tools format),
#    with the offsets PlatformIO itself flashes them at. Each path carries a
#    hash of the file (?v=...), so browsers never install a cached old image.
#
# `pio run` (all environments) leaves every board's image in build/.

import hashlib
import json
import os
import shutil

Import("env")

firmware_name = env.GetProjectOption("custom_ota_firmware", "")
firmware_version = env.GetProjectOption("custom_firmware_version", "dev")

env.Append(CPPDEFINES=[("FIRMWARE_VERSION", env.StringifyMacro(firmware_version))])

# ESP Web Tools chip family for each MCU.
CHIP_FAMILIES = {
    "esp32": "ESP32",
    "esp32s2": "ESP32-S2",
    "esp32s3": "ESP32-S3",
    "esp32c3": "ESP32-C3",
}

if firmware_name:
    env.Append(CPPDEFINES=[("OTA_FIRMWARE_FILE", env.StringifyMacro(firmware_name))])

    output_dir = os.path.join(env.subst("$PROJECT_DIR"), "build")
    board_dir_name = os.path.splitext(firmware_name)[0]

    def copy_ota_image(source, target, env):
        os.makedirs(output_dir, exist_ok=True)
        destination = os.path.join(output_dir, firmware_name)
        shutil.copyfile(target[0].get_abspath(), destination)
        version_file = os.path.join(output_dir, board_dir_name + ".version")
        with open(version_file, "w") as fp:
            fp.write(firmware_version + "\n")
        print("OTA image: %s (version %s)" % (destination, firmware_version))

    # "<path>?v=<first 12 hex of the file's SHA-256>": a new image gets a new
    # URL, which the browser cannot answer from its cache.
    def versioned_path(relative_path):
        with open(os.path.join(output_dir, relative_path), "rb") as fp:
            digest = hashlib.sha256(fp.read()).hexdigest()[:12]
        return relative_path + "?v=" + digest

    # Copies the extra images to build/<board>/ and returns the manifest parts
    # (paths relative to build/), or None when one is missing.
    def copy_flash_parts(env):
        board_dir = os.path.join(output_dir, board_dir_name)
        os.makedirs(board_dir, exist_ok=True)
        parts = []
        for offset, path in env.get("FLASH_EXTRA_IMAGES", []):
            path = env.subst(path)
            if not os.path.isfile(path):
                print("Web flasher: missing %s, %s not added to the manifest" % (path, board_dir_name))
                return None
            name = os.path.basename(path)
            shutil.copyfile(path, os.path.join(board_dir, name))
            parts.append({"path": versioned_path(board_dir_name + "/" + name),
                          "offset": int(env.subst(offset), 0)})
        app_offset = int(env.subst("$ESP32_APP_OFFSET") or "0x10000", 0)
        parts.append({"path": versioned_path(firmware_name), "offset": app_offset})
        return sorted(parts, key=lambda part: part["offset"])

    # Adds or replaces this board's build in build/manifest.json, keeping the
    # other boards (each environment builds on its own).
    def update_manifest(source, target, env):
        mcu = env.BoardConfig().get("build.mcu", "").lower()
        chip_family = CHIP_FAMILIES.get(mcu)
        if not chip_family:
            print("Web flasher: unknown chip %s, not added to the manifest" % mcu)
            return
        parts = copy_flash_parts(env)
        if parts is None:
            return

        manifest_path = os.path.join(output_dir, "manifest.json")
        manifest = {}
        if os.path.isfile(manifest_path):
            try:
                with open(manifest_path) as fp:
                    manifest = json.load(fp)
            except ValueError:
                manifest = {}
        builds = [b for b in manifest.get("builds", []) if b.get("chipFamily") != chip_family]
        builds.append({"chipFamily": chip_family, "parts": parts})
        manifest = {
            "name": "MokaBot",
            "version": firmware_version,
            # Asked on every install: erasing wipes WiFi, settings,
            # calibration and saved brews.
            "new_install_prompt_erase": True,
            "builds": sorted(builds, key=lambda b: b["chipFamily"]),
        }
        with open(manifest_path, "w") as fp:
            json.dump(manifest, fp, indent=2)
            fp.write("\n")
        print("Web flasher: %s added to %s" % (chip_family, manifest_path))

    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_ota_image)
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", update_manifest)
