# Reading settings with every failure handled: a missing file, bad lines, bad numbers.
import os

NUMERIC = {"port", "workers", "timeout"}


def load(path):
    settings, errors = {}, []
    with open(path) as f:
        for i, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            key, sep, value = line.partition("=")
            if not sep:
                errors.append(f"line {i}: expected key = value")
                continue
            key, value = key.strip(), value.strip()
            if key not in NUMERIC:
                settings[key] = value
                continue
            try:
                settings[key] = int(value)
            except ValueError:
                errors.append(f"line {i}: {key} is not a number: {value}")
    return settings, errors


path = "settings.conf"
with open(path, "w") as f:
    f.write("# server settings\nport = 8080\nhost = example.org\nworkers = many\ntimeout = 30\nbad line\n")
try:
    for name in ["missing.conf", path]:
        try:
            settings, errors = load(name)
        except OSError:
            print(f"{name}: using defaults")
            continue
        for k, v in settings.items():
            print(k, v)
        for e in errors:
            print("error:", e)
finally:
    os.remove(path)
