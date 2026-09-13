"""Shared SPY1 pinyin-index constants.

These values must stay in lockstep with main/stroke_order/stroke_order_pinyin.h.
"""

MAGIC = b"SPY1"
FORMAT_VERSION = 1

HEADER_SIZE = 32
CHAR_INDEX_ENTRY_SIZE = 12
GROUP_INDEX_ENTRY_SIZE = 8
READING_SIZE = 2
MEMBER_SIZE = 8

# SPY1 was not externally released. The 2000-character prototype keeps the v1
# byte layout and raises only validator bounds after measuring the corpus:
# 2000 characters, 1047 groups, max group 29, 63,618 bytes.
MAX_CHARACTERS = 2048
MAX_GROUPS = 2048
MAX_READINGS_PER_CHARACTER = 8
MAX_GROUP_MEMBERS = 32
MAX_FILE_BYTES = 64 * 1024

MIN_TARGET_CODEPOINT = 0x4E00
MAX_TARGET_CODEPOINT = 0x9FFF

U32_MAX = 0xFFFFFFFF

HEADER_STRUCT_FORMAT = "<4sHHIIIIII"
CHAR_INDEX_STRUCT_FORMAT = "<IHBBI"
GROUP_INDEX_STRUCT_FORMAT = "<HHI"
MEMBER_STRUCT_FORMAT = "<IHH"
READING_STRUCT_FORMAT = "<H"

PINYIN_SOURCE = "Unicode 16.0.0 Unihan_Readings.txt"
PINYIN_FIELDS = ("kMandarin", "kHanyuPinyin")
UNICODE_LICENSE = "Unicode License v3"
