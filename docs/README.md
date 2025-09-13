# Documentation

This repository can generate API documentation from the source code using Doxygen.

- Build: `bazelisk build //docs:site`
- Open: `bazel-bin/docs/site_html/html/index.html`

Notes
- Requires `doxygen` on your PATH. Install via your package manager.
- The docs include source browsing and cross-references (who-references / referenced-by).
- CUDA sources (`.cu/.cuh`) are mapped to C++ for parsing.

