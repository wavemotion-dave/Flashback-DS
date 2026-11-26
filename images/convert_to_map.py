import os
from PIL import Image

PNG_FILE = "custom_screen.png"
MAP_OUT = "instru_e_new.map"
PAL_OUT = "instru_e_new.pal"

W, H = 256, 224

# --- Load and quantize ---
img = Image.open(PNG_FILE)
if img.mode == "P" and img.size == (W, H):
    indexed_img = img  # already indexed at the right size, no quality loss
else:
    indexed_img = (
        img.convert("RGB")
        .resize((W, H), Image.LANCZOS)
        .quantize(colors=256)
    )
linear_pixels = indexed_img.tobytes()

# --- Pack linear pixels into 4 Mode X planes (each 64x224 = 14,336 bytes) ---
planar_pixels = bytearray()
for plane in range(4):
    for y in range(H):
        row_start = y * W
        for x in range(64):
            planar_pixels.append(linear_pixels[row_start + x * 4 + plane])

with open(MAP_OUT, "wb") as f:
    f.write(planar_pixels)

# --- 768-byte 8-bit palette, padded if the PNG's palette is short ---
raw_pal = list(indexed_img.getpalette() or [])[:768]
raw_pal += [0] * (768 - len(raw_pal))

with open(PAL_OUT, "wb") as f:
    f.write(bytes(raw_pal))

print("Wrote:", os.path.abspath(MAP_OUT), os.path.getsize(MAP_OUT), "bytes (expect 57344)")
print("Wrote:", os.path.abspath(PAL_OUT), os.path.getsize(PAL_OUT), "bytes (expect 768)")
