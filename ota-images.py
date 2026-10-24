# PlatformIO extra script (pre): OTA firmware images per board.
#
# Each environment names its OTA image with `custom_ota_firmware` in
# platformio.ini. This script:
#  - passes that name to the code as OTA_FIRMWARE_FILE, so the device
#    downloads <OTA folder URL>/<name>;
#  - after every build, copies firmware.bin to build/<name> in the project
#    folder, ready to publish in the OTA folder.
#
# `pio run` (all environments) leaves every board's image in build/.

import os
import shutil

Import("env")

firmware_name = env.GetProjectOption("custom_ota_firmware", "")

if firmware_name:
    env.Append(CPPDEFINES=[("OTA_FIRMWARE_FILE", env.StringifyMacro(firmware_name))])

    output_dir = os.path.join(env.subst("$PROJECT_DIR"), "build")

    def copy_ota_image(source, target, env):
        os.makedirs(output_dir, exist_ok=True)
        destination = os.path.join(output_dir, firmware_name)
        shutil.copyfile(target[0].get_abspath(), destination)
        print("OTA image: %s" % destination)

    env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", copy_ota_image)
