# PlatformIO extra script: build every C++ file as gnu++17.
#
# The Arduino-ESP32 platform puts -std=gnu++2b and -std=gnu++2a into the C++
# flags (the last one wins). The UI is written and tested (simulator) as
# C++17; building it as C++20 could change behaviour (for example the type of
# u8"" literals). This script removes the platform's -std=gnu++ options and
# appends -std=gnu++17 as the last C++ standard flag, in the project
# environment, the library environments and the shared default one.
from SCons.Script import DefaultEnvironment

Import("env")
try:
    Import("projenv")
except Exception:  # pre-6.x layouts have no separate project environment
    projenv = env


def flatten(items):
    for item in items:
        if isinstance(item, (list, tuple)):
            yield from flatten(item)
        else:
            yield item


def fix(e):
    cxx = [f for f in flatten(e.get("CXXFLAGS", [])) if not str(f).startswith("-std=gnu++")]
    e.Replace(CXXFLAGS=cxx)
    e.Append(CXXFLAGS=["-std=gnu++17"])


for e in (env, projenv, DefaultEnvironment()):
    fix(e)


# The library builders (LVGL, ArduinoJson, ui) work on their own copies.
for lb in env.GetLibBuilders():
    fix(lb.env)
