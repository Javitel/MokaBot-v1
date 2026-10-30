# PlatformIO extra script (pre): gzip-compressed copies of the web pages.
#
# The device serves data/brew.html, data/index.html and data/settings.html
# from the firmware (board_build.embed_files in platformio.ini). Embedding them
# gzip-compressed saves ~25 KB of flash; the web server sends them with
# "Content-Encoding: gzip" and the browser unpacks them. data/ keeps the
# readable originals (also bundled as they are by the Android app).
#
# Writes .pio/pages/<name>.html.gz before every build, only when the page
# changed (mtime=0: the same page always gives the same bytes).

import gzip
import os

Import("env")

PAGES = ("brew.html", "index.html", "settings.html")

project_dir = env.subst("$PROJECT_DIR")
output_dir = os.path.join(project_dir, ".pio", "pages")
os.makedirs(output_dir, exist_ok=True)

for page in PAGES:
    with open(os.path.join(project_dir, "data", page), "rb") as fp:
        data = gzip.compress(fp.read(), compresslevel=9, mtime=0)
    target = os.path.join(output_dir, page + ".gz")
    if os.path.isfile(target):
        with open(target, "rb") as fp:
            if fp.read() == data:
                continue
    with open(target, "wb") as fp:
        fp.write(data)
    print("Web page: %s gzipped to %d bytes" % (page, len(data)))
