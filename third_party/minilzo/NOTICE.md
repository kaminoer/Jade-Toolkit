# Vendored: miniLZO 2.09

`minilzo.c`, `minilzo.h`, `lzoconf.h`, `lzodefs.h` are **miniLZO 2.09**
(`MINILZO_VERSION 0x2090`), the official minimal subset of the LZO real-time
data-compression library by Markus F.X.J. Oberhumer.

- Source: https://github.com/yuhaoth/minilzo (mirror of the upstream
  http://www.oberhumer.com/opensource/lzo/ release).
- License: **GNU GPL v2** — see `COPYING`. This makes the `jade_native` binary
  GPL-licensed. Acceptable for this personal, unreleased modding toolkit; the
  Python toolkit it replaces already links python-lzo (also LZO/GPL).

## Why this version

The Jade engine tree ships miniLZO **1.06** (1999), which emits
`-Wpointer-to-int-cast` truncation warnings on a 64-bit build (its `lzo_ptr_t`
is 32-bit). 2.09 is 64-bit-clean and compiles with `-Wall -Wextra` with no
warnings — and the pointer arithmetic matters for the *compress* path we'll add
later, not just decompress.

## Why this is still correct for v37/v38

LZO1X is a fixed algorithm; **decompression output is identical** across all LZO
versions and all BigFile versions. The BF-specific block *framing* lives in
`src/Compression.cpp` (ported from `jade_explorer/core/compression.py`), not
here. Correctness is proven empirically: `tests/run_golden.py` byte-diffs C++
decompression against python-lzo on the real v36/v37/v38 archives.

We use only `lzo_init`, `lzo1x_decompress_safe` (read path), and later
`lzo1x_1_compress` (write path). Files are vendored verbatim, unmodified.
