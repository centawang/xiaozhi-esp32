"""Host-side stroke-order conversion and validation."""

from .convert import ConvertError, convert_dataset, report_charset_coverage
from .path import PathError
from .pinyin import PinyinError, StrokePinyinBlob, normalize_pinyin, pack_spy1, validate_spy1
from .select import SelectionError, load_selection_csv
from .unpack import StrokeOrderBlob, ValidationError, validate_blob

__all__ = [
    "ConvertError",
    "PathError",
    "PinyinError",
    "SelectionError",
    "StrokeOrderBlob",
    "StrokePinyinBlob",
    "ValidationError",
    "convert_dataset",
    "load_selection_csv",
    "normalize_pinyin",
    "pack_spy1",
    "report_charset_coverage",
    "validate_blob",
    "validate_spy1",
]
