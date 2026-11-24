"""Generate original CUR frames wrapped in a RIFF animated cursor."""

from pathlib import Path
from struct import pack
import sys


SIZE = 16
HOTSPOT = (3, 5)


def chunk(tag, payload):
    return tag + pack('<I', len(payload)) + payload + (b'\0' if len(payload) % 2 else b'')


def cursor_frame(bgra):
    pixels = bytes(bgra) * SIZE * SIZE
    row_bytes = (SIZE + 7) // 8
    mask_pitch = (row_bytes + 3) & ~3
    mask = (bytes(row_bytes) + b'\xff' * (mask_pitch - row_bytes)) * SIZE
    dib = pack('<IiiHHIIiiII', 40, SIZE, 2 * SIZE, 1, 32, 0, len(pixels) + len(mask), 0, 0, 0, 0)
    header = pack('<HHHBBBBHHII', 0, 2, 1, SIZE, SIZE, 0, 0, *HOTSPOT,
                  len(dib) + len(pixels) + len(mask), 22)
    return header + dib + pixels + mask


def animated_cursor(colors):
    count = len(colors)
    header = pack('<9I', 36, count, count, 0, 0, 0, 0, 1, 1)
    frames = b'fram' + b''.join(chunk(b'icon', cursor_frame(color)) for color in colors)
    rates = pack('<I', 0xffffffff) if count == 1 else b''.join(pack('<I', 6 * (index + 1)) for index in range(count))
    return chunk(b'RIFF', b'ACON' + chunk(b'anih', header) + chunk(b'rate', rates) + chunk(b'LIST', frames))


def initializer(name, data):
    lines = [f'alignas(DWORD) static const BYTE {name}[] = {{']
    for start in range(0, len(data), 16):
        lines.append('    ' + ', '.join(f'0x{byte:02x}' for byte in data[start:start + 16]) + ',')
    return '\n'.join([*lines, '};', ''])


if __name__ == '__main__':
    colors = ((0, 0, 255, 255), (255, 0, 0, 255))
    Path(sys.argv[1]).write_text(
        f'enum {{ CURSOR_SIZE = {SIZE}, HOTSPOT_X = {HOTSPOT[0]}, HOTSPOT_Y = {HOTSPOT[1]} }};\n'
        + initializer('singleFrameCursor', animated_cursor(colors[:1]))
        + initializer('animatedCursor', animated_cursor(colors)), encoding='ascii')
