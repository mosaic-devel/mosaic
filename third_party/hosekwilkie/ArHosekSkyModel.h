/*
This source is published under the following 3-clause BSD license.

Copyright (c) 2012 - 2013, Lukas Hosek and Alexander Wilkie
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * None of the names of the contributors may be used to endorse or promote
      products derived from this software without specific prior written
      permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/* ============================================================================

This file is part of a sample implementation of the analytical skylight and
solar radiance models presented in the SIGGRAPH 2012 paper


           "An Analytic Model for Full Spectral Sky-Dome Radiance"

and the 2013 IEEE CG&A paper

       "Adding a Solar Radiance Function to the Hosek Skylight Model"

                                   both by

                       Lukas Hosek and Alexander Wilkie
                Charles University in Prague, Czech Republic


                        Version: 1.4a, February 22nd, 2013

============================================================================ */

/* MOSAIC VENDORING NOTE (docs/texture-generator.md §4.1/§9.2; the BLAKE3 portable-lanes-only
   precedent): trimmed from the upstream v1.4a ArHosekSkyModel.h/.c to the RGB lane only --
   the spectral and CIE XYZ datasets (~580 KB of coefficients) and the alien-world / spectral
   solar-radiance functions serve nothing Mosaic calls and are deliberately not vendored. The
   functions below are verbatim upstream, per the BSD notice above. If the spectral lane is
   ever wanted (spectral solar disc, S55-g HDR export), drop the upstream files back in
   beside these. Upstream: https://cgg.mff.cuni.cz/projects/SkylightModelling/ */

#ifndef _ARHOSEK_SKYMODEL_H_
#define _ARHOSEK_SKYMODEL_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef double ArHosekSkyModelConfiguration[9];

/* ----------------------------------------------------------------------------

    ArHosekSkyModelState struct
    ---------------------------

    This struct holds the pre-computation data for one particular albedo value.
    Most fields are self-explanatory, but users should never directly
    manipulate any of them anyway. The only consistent way to manipulate such
    structs is via the functions 'arhosek_rgb_skymodelstate_alloc_init' and
    'arhosekskymodelstate_free'.

---------------------------------------------------------------------------- */

typedef struct ArHosekSkyModelState
{
    ArHosekSkyModelConfiguration  configs[11];
    double                        radiances[11];
    double                        turbidity;
    double                        solar_radius;
    double                        emission_correction_factor_sky[11];
    double                        emission_correction_factor_sun[11];
    double                        albedo;
    double                        elevation;
}
ArHosekSkyModelState;

void arhosekskymodelstate_free(
        ArHosekSkyModelState  * state
        );

/*   RGB version of the model: sRGB primaries with a linear gamma ramp.
     Turbidity in [1, 10], ground albedo in [0, 1], solar elevation in
     radians in [0, pi/2].                                                    */

ArHosekSkyModelState  * arhosek_rgb_skymodelstate_alloc_init(
        const double  turbidity,
        const double  albedo,
        const double  elevation
        );

/*   Sky dome radiance for the direction (theta = angle to zenith, gamma =
     angle to the sun, both radians); channel is 0/1/2 = R/G/B.               */

double arhosek_tristim_skymodel_radiance(
        ArHosekSkyModelState  * state,
        double                  theta,
        double                  gamma,
        int                     channel
        );

#ifdef __cplusplus
}
#endif

#endif // _ARHOSEK_SKYMODEL_H_
