from __future__ import annotations

import importlib
import importlib.util
from typing import Any, Optional


def load_psutil() -> Optional[Any]:
    """Return the psutil module if available."""

    if importlib.util.find_spec("psutil") is None:
        return None
    return importlib.import_module("psutil")
