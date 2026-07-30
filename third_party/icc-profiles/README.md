# Vendored ICC profiles

## ISOcoated_v2_300_eci.icc

The default CMYK profile for Mosaic's colour-managed CMYK picker model (PLAN S12-b): **ISO Coated
v2 300% (ECI)** — offset printing per ISO 12647-2:2004 on coated paper, characterization data
**FOGRA39L**, 300% total ink limit. md5 `e14f5db955711d914d877df35ad7a1b5`.

- **Provenance:** extracted unmodified from Debian's `icc-profiles_2.1.orig.tar.gz`
  (`pool/non-free/i/icc-profiles/`), 2026-06-10. Upstream author: European Color Initiative /
  Heidelberger Druckmaschinen AG.
- **Licence:** the **HEIDELBERG ICC profile licence** (see `LICENSE`, shipped verbatim as it
  requires): redistribution is explicitly permitted, including bundled with other software,
  provided the licence text accompanies the profile, **no fee is charged for the profile itself**,
  and the profile is **not modified**. Mosaic is gratis and ships the file untouched.
- **Caveat for packagers:** the no-modification/no-fee terms are not DFSG-/FSF-free (Debian ships
  these profiles in *non-free*). The profile is a separate data file, not part of Mosaic's GPLv3
  program code; a distribution that objects can delete it — Mosaic's CMYK picker model then
  falls back to any user-supplied CMYK profile (S12-c) and hides when none is available.

Do **not** add profiles from eci.org's website here without checking terms: ECI's *web download*
notices are more restrictive than the HEIDELBERG licence embedded in this Debian-distributed set.
