"""Bounded native stdout/stderr capture without backpressure on compute tasks."""

import os
import threading
from pathlib import Path

# Protocol storage bound, not a tunable configuration default. Retain the current
# segment and one previous segment for diagnostics (at most 16 MiB per attempt).
SEGMENT_BYTES = 8 * 1024 * 1024


class Capture:
    def __init__(self, root, source):
        self.path = Path(root) / "task.log"
        self.source = source
        self.error = None
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def _run(self):
        output = None
        try:
            output = self.path.open("xb", buffering=0)
            size = 0
            while True:
                chunk = self.source.read(65536)
                if not chunk:
                    break
                if size + len(chunk) > SEGMENT_BYTES:
                    output.close()
                    output = None
                    os.replace(self.path, self.path.with_suffix(".log.1"))
                    output = self.path.open("xb", buffering=0)
                    size = 0
                output.write(chunk)
                size += len(chunk)
        except OSError as exc:
            self.error = str(exc)
            # A full filesystem must not leave the child blocked in write(2).
            try:
                while self.source.read(65536):
                    pass
            except OSError:
                pass
        finally:
            if output is not None:
                output.close()
            self.source.close()

    def finish(self, timeout):
        self.thread.join(timeout)
        if self.thread.is_alive():
            return "Native stdout remains open after descendant retirement"
        return self.error
