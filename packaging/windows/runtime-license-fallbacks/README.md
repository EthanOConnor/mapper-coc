# Windows runtime license fallbacks

The runtime-license collector normally uses license files owned by each MSYS2
DLL package. A curated fallback is allowed only when the binary package omits
license files and the collector can verify the expected package metadata.

- `mingw-w64-x86_64-libidn2`: MSYS2 declares
  `GPL-2.0-or-later OR LGPL-3.0-or-later`. The collector selects GPLv3 under
  the `GPL-2.0-or-later` option and uses the repository's `COPYING` file.
- `mingw-w64-x86_64-lz4` version 1.10.0: the staged DLL is the BSD-licensed
  LZ4 library. `lz4-1.10.0-BSD-2-Clause.txt` is the upstream `lib/LICENSE`
  from <https://github.com/lz4/lz4/tree/v1.10.0/lib>, SHA-256
  `8b58c446121a109ccf32edc094bba3010a3d85e4ee3702950db55e4d3e87736c`.

Any version or license-metadata mismatch leaves the package unresolved and
fails the packaging job.
