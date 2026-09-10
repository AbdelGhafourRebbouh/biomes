"""Regenerate the Windows ICO from the original vector artwork.

Developer-only dependencies: PyMuPDF and Pillow. Not needed to build biomes.
"""
from pathlib import Path
import xml.etree.ElementTree as ET
import pymupdf
from PIL import Image

root = Path(__file__).resolve().parents[1]
source = root / "frontend/icons/icon biomes taskbaar.svg"
svg = ET.parse(source).getroot()
# Resolve percentage dimensions against the square artboard, not a page size.
svg.set("width", "1024")
svg.set("height", "1024")
document = pymupdf.open(stream=ET.tostring(svg), filetype="svg")
pixmap = document[0].get_pixmap(alpha=True)
image = Image.frombytes("RGBA", (pixmap.width, pixmap.height), pixmap.samples)
destination = root / "frontend/icons/biomes.ico"
image.save(destination, format="ICO", sizes=[(s, s) for s in (16, 20, 24, 32, 40, 48, 64, 128, 256)])
print(destination)
