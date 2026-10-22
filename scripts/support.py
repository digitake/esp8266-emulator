"""Shared process handling and source provenance for command-line tools."""
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def source_commit(override=None):
    """Prefer an explicit commit, then EMU_COMMIT, then this checkout's HEAD."""
    if override or os.environ.get('EMU_COMMIT'):
        return override or os.environ['EMU_COMMIT']
    try:
        return subprocess.check_output(
            ['git', '-C', str(ROOT), 'rev-parse', 'HEAD'],
            stderr=subprocess.DEVNULL, text=True, timeout=5).strip()
    except (OSError, subprocess.SubprocessError):
        return None


def cli(main):
    """Print concise failures without echoing unmasked guest stdout/stderr."""
    try:
        return main() or 0
    except subprocess.CalledProcessError as error:
        print('error: %s exited with status %d' %
              (Path(error.cmd[0]).name, error.returncode), file=sys.stderr)
    except subprocess.TimeoutExpired as error:
        print('error: %s timed out after %g seconds' %
              (Path(error.cmd[0]).name, error.timeout), file=sys.stderr)
    except (OSError, ValueError, KeyError) as error:
        print('error: %s' % error, file=sys.stderr)
    return 1
