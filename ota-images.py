# PlatformIO extra script (pre): OTA firmware images per board.
#
# Each environment names its OTA image with `custom_ota_firmware` in
# platformio.ini, and `custom_firmware_version` sets the release version.
# This script:
#  - passes them to the code as OTA_FIRMWARE_FILE and FIRMWARE_VERSION, so
#    the device downloads <OTA folder URL>/<name> and knows its version;
#  - after every build, copies firmware.bin to build/<name> and writes the
#    version to build/<name without .bin>.version, which the device reads to
#    tell whether the published image is newer.
#
# `pio run` (all environments) leaves every board's image in build/.

import os
import shutil

Import("env")

firmware_name = env.GetProjectOption("custom_ota_firmware", "")
firmware_version = env.GetProjectOption("custom_firmware_version", "dev")

env.Append(CPPDEFINES=[("FIRMWARE_VERSION", env.StringifyMacro(firmware_version))])

if firmware_name:
    env.Append(CPPDEFINES=[("OTA_FIRMWARE_FILE", env.StringifyMacro(firmware_name))])

    output_dir = os.path.join(env.subst("$PROJECT_DIR"), "build")

    def copy_ota_image(source, target, env):
        os.makedirs(output_dir, exist_ok=True)
        destination = os.path.join(output_dir, firmware_name)
        shutil.copyfile(target[0].get_abspath(), destination)
        version_file = os.path.join(output_dir, os.path.splitext(firmware_name)[0] + ".version")
        with open(version_file, "w") as fp:
            fp.write(firmware_version + "\n")
        print("OTA image: %s (version %s)" % (destination, firmware_version))

    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_ota_image)
