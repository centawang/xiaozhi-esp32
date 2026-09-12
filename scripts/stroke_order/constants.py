"""Shared SOB1 stroke-order binary constants.

These values must stay in lockstep with main/stroke_order/stroke_order_store.h.
Host tests compare the C++ constexpr names against this module.
"""

MAGIC = b"SOB1"
FORMAT_VERSION = 1
COORD_MAX = 1024

HEADER_SIZE = 32
INDEX_ENTRY_SIZE = 16
CHARACTER_RECORD_HEADER_SIZE = 8
STROKE_HEADER_SIZE = 4
POINT_SIZE = 4

MAX_CHARACTERS = 1024
MAX_STROKES_PER_CHARACTER = 48
MAX_OUTLINE_POINTS_PER_STROKE = 256
MAX_MEDIAN_POINTS_PER_STROKE = 64
# Host packing budget so a 500-character prototype stays within MAX_FILE_BYTES
# without raising the on-device outline or file limits.
PACK_OUTLINE_POINTS_PER_STROKE = 200
MAX_CHARACTER_BYTES = 16 * 1024
MAX_FILE_BYTES = 1024 * 1024

MIN_STROKES_PER_CHARACTER = 1
MIN_OUTLINE_POINTS_PER_STROKE = 4
MIN_MEDIAN_POINTS_PER_STROKE = 2

FLATTEN_TOLERANCE = 0.25
FLATTEN_MAX_DEPTH = 8

U32_MAX = 0xFFFFFFFF
MIN_TARGET_CODEPOINT = 0x4E00
MAX_TARGET_CODEPOINT = 0x9FFF

# The last two fields are header_crc32 and index_crc32 respectively.
HEADER_STRUCT_FORMAT = "<4sHHIIIIII"
INDEX_STRUCT_FORMAT = "<IIII"
CHAR_HEADER_STRUCT_FORMAT = "<IHH"
STROKE_HEADER_STRUCT_FORMAT = "<HH"
POINT_STRUCT_FORMAT = "<HH"

SOURCE_PROJECT = "hanzi-writer-data"
SOURCE_UPSTREAM = "skishore/makemeahanzi"
DEFAULT_SOURCE_REPO = "https://github.com/chanind/hanzi-writer-data"
GRAPHICS_LICENSE = "Arphic Public License"
DICTIONARY_LICENSE_NOTE = (
    "Dictionary material associated with Make Me a Hanzi / related datasets may be "
    "LGPL or other copyleft terms. This converter only reads strokes/medians graphics "
    "JSON and does not vendor dictionary text."
)
