"""Import bridge for the command line script's hyphenated filename."""
import importlib.util
from pathlib import Path

_spec = importlib.util.spec_from_file_location("_glidebeam_ci_version", Path(__file__).with_name("ci-version.py"))
assert _spec is not None and _spec.loader is not None
_module = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_module)
release_version = _module.release_version
