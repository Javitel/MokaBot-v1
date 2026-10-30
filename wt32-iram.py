# PlatformIO extra script (pre), WT32-SC01 only: frees IRAM for Bluetooth.
#
# The classic ESP32 has 128 KB of IRAM, and the Bluetooth controller does not
# fit next to WiFi with the precompiled framework's placement. That framework
# is built with the PSRAM cache workaround options that put parts of the C
# library in IRAM for speed (CONFIG_SPIRAM_CACHE_LIBTIME_IN_IRAM...). The time
# functions (mktime, strftime, strptime, tzset...) are rarely called here and
# never from interrupts, so this script links them from flash instead: it
# writes a copy of the framework's sections.ld without their placement lines
# and puts it first in the linker search path (the framework passes
# "-T sections.ld" by name). No framework rebuild needed.

import os
import re

Import("env")

TIME_OBJECTS = re.compile(
    r"\*libc\.a:libc_a-(asctime|asctime_r|ctime|ctime_r|gmtime|gmtime_r|lcltime|lcltime_r|"
    r"localtime|localtime_r|mktime|strftime|strptime|tzcalc_limits|tzset|tzset_r|"
    r"gettzinfo|difftime)\.\*"
)
# A placement line: "    *libc.a:libc_a-mktime.*(.literal ...)".
PLACEMENT_LINE = re.compile(r"^\s*" + TIME_OBJECTS.pattern + r"\(")

framework_libs = env.PioPlatform().get_package_dir("framework-arduinoespressif32-libs")
source = os.path.join(framework_libs, env.BoardConfig().get("build.mcu"), "ld", "sections.ld")
output_dir = os.path.join(env.subst("$BUILD_DIR"), "ld-iram")
os.makedirs(output_dir, exist_ok=True)

with open(source) as fp:
    lines = fp.readlines()
# Their IRAM / DRAM placement lines go, and they are no longer excluded from
# the flash sections (EXCLUDE_FILE lists), or the linker would place them as
# orphan sections and break the flash layout.
kept = []
for line in lines:
    if PLACEMENT_LINE.match(line):
        continue
    kept.append(re.sub(r" ?" + TIME_OBJECTS.pattern, "", line) if "EXCLUDE_FILE" in line else line)
with open(os.path.join(output_dir, "sections.ld"), "w") as fp:
    fp.writelines(kept)

env.Prepend(LIBPATH=[output_dir])
print("IRAM: libc time functions linked from flash (%d placement lines removed)" % (len(lines) - len(kept)))
