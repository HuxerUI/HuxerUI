"""Generate range transitions for grapheme segmentation and paragraph direction."""

import sys
from pathlib import Path


def records(lines):
    for line in lines:
        fields = line.partition("#")[0].strip().split(";")
        if len(fields) < 2:
            continue
        start, _, end = fields[0].strip().partition("..")
        yield int(start, 16), int(end or start, 16), tuple(field.strip() for field in fields[1:])


def grapheme_properties(root):
    values = bytearray(0x110000)
    breaks = ("Other", "CR", "LF", "Control", "Extend", "ZWJ", "Regional_Indicator",
              "Prepend", "SpacingMark", "L", "V", "T", "LV", "LVT")
    break_path = root / "auxiliary/GraphemeBreakProperty.txt"
    for start, end, (value,) in records(break_path.read_text(encoding="utf-8").splitlines()):
        if value not in ("LV", "LVT"):
            values[start:end + 1] = bytes([breaks.index(value)]) * (end - start + 1)

    core_lines = (root / "DerivedCoreProperties.txt").read_text(encoding="utf-8").splitlines()
    for start, end, properties in records(core_lines):
        if properties[0] == "InCB":
            indic = ("None", "Consonant", "Linker", "Extend").index(properties[1])
            for code_point in range(start, end + 1):
                values[code_point] |= indic << 4

    emoji_lines = (root / "emoji/emoji-data.txt").read_text(encoding="utf-8").splitlines()
    for start, end, (value,) in records(emoji_lines):
        if value == "Extended_Pictographic":
            for code_point in range(start, end + 1):
                values[code_point] |= 1 << 6

    return values


def paragraph_direction_properties(root):
    values = bytearray(0x110000)
    lines = (root / "extracted/DerivedBidiClass.txt").read_text(encoding="utf-8").splitlines()
    directions = {"L": 1, "R": 2, "AL": 2, "LRI": 3, "RLI": 3, "FSI": 3, "PDI": 4, "B": 5}
    default_directions = {"Left_To_Right": 1, "Right_To_Left": 2, "Arabic_Letter": 2}
    defaults = (line.removeprefix("# @missing:") for line in lines if line.startswith("# @missing:"))
    for start, end, (value,) in records(defaults):
        values[start:end + 1] = bytes([default_directions.get(value, 0)]) * (end - start + 1)
    for start, end, (value,) in records(lines):
        values[start:end + 1] = bytes([directions.get(value, 0)]) * (end - start + 1)
    return values


def print_table(name, values):
    transitions = [
        f"0x{(index << 8) | entry:08X}U,"
        for index, entry in enumerate(values)
        if index == 0 or entry != values[index - 1]
    ]
    print(f"constexpr std::uint32_t {name}[] = {{")
    for index in range(0, len(transitions), 6):
        print("    " + " ".join(transitions[index:index + 6]))
    print("};")


if __name__ == "__main__":
    root = Path(sys.argv[1])
    print("// Generated from Unicode Character Database data. See 3dparty/unicode/METADATA.")
    print("// Each entry encodes a range start above the low byte of property values.")
    print_table("grapheme_properties", grapheme_properties(root))
    print()
    print_table("paragraph_direction_properties", paragraph_direction_properties(root))
