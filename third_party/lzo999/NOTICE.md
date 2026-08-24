# lzo1x_999 (vendored subset of LZO 2.10)

Source: https://www.oberhumer.com/opensource/lzo/ (lzo-2.10.tar.gz)
License: GPLv2 (see COPYING) — same license family as the miniLZO 2.09
subset already vendored at ../minilzo.

Why: python-lzo's `lzo.compress(block, 9, False)` — what the toolkit's
`compress_lzo_9` calls — maps every level > 1 to `lzo1x_999_compress`.
miniLZO deliberately ships only `lzo1x_1_compress`, so the native write
paths could previously only match the Python oracle at the DECOMPRESSED
level. With the real 999 optimizer the compressed archives are
byte-identical too.

Vendored files (unmodified):
- src/: lzo1x_9x.c + its includes (lzo_mchw.ch, lzo_swd.ch, config1x.h,
  lzo_conf.h, lzo_dict.h, lzo_supp.h, lzo_func.h, lzo_ptr.h)
- include/lzo/: lzo1x.h, lzoconf.h, lzodefs.h

Added: lzo999_wrap.c — a size_t-based wrapper (`jade_lzo1x_999_compress`)
so libjade never has to include LZO headers or reason about `lzo_uint`.
The 999 path is pure integer code with no FP and no external LZO objects
(lzo_init/lzo_util are not referenced), so it coexists with miniLZO
without symbol clashes: lzo1x_9x.c exports only `lzo1x_999_compress*`.
