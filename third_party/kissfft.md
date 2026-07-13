<!--
SPDX-License-Identifier: BSD-3-Clause
SPDX-FileCopyrightText: 2003-2010 Mark Borgerding <Mark@Borgerding.net>
-->
# KissFFT

This project uses KissFFT (<https://github.com/mborgerding/kissfft>), tag 131.1.0.

KissFFT is licensed under the BSD 3-Clause License (see `LICENSES/BSD-3-Clause.txt`).
It is fetched at build time via CMake FetchContent and is not checked into this
repository.  Only `kiss_fft.c` is compiled; `kiss_fftr` is not used.

**Usage in kairos-grid**: the `KAIROS_GRID_BUILD_FFT` CMake option enables
`kairos-grid-kissfft` (a static library wrapping just `kiss_fft.c`) and
`kairos_grid::fft-modules` (the INTERFACE target linking engine + kissfft).
`FftModule` (`include/kairos_grid/fft/fft_module.hpp`) feeds real audio samples
as complex values with zero imaginary parts to the standard `kiss_fft()` API,
producing an N-point magnitude spectrum used for centroid, flatness, and flux
spectral descriptors.
