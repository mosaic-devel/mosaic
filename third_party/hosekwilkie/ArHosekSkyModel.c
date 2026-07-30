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

/* MOSAIC VENDORING NOTE: trimmed from the upstream v1.4a ArHosekSkyModel.c to the RGB lane
   only (see the note in ArHosekSkyModel.h). Every function body below is verbatim upstream. */

#include "ArHosekSkyModel.h"
#include "ArHosekSkyModelData_RGB.h"
#include <math.h>
#include <stdlib.h>

#ifndef MATH_PI
#define MATH_PI                     3.141592653589793
#endif

#ifndef MATH_DEG_TO_RAD
#define MATH_DEG_TO_RAD             ( MATH_PI / 180.0 )
#endif

#ifndef DEGREES
#define DEGREES                     * MATH_DEG_TO_RAD
#endif

#ifndef TERRESTRIAL_SOLAR_RADIUS
#define TERRESTRIAL_SOLAR_RADIUS    ( ( 0.51 DEGREES ) / 2.0 )
#endif

#ifndef ALLOC
#define ALLOC(_struct)              ((_struct *)malloc(sizeof(_struct)))
#endif

// internal definitions

typedef double *ArHosekSkyModel_Dataset;
typedef double *ArHosekSkyModel_Radiance_Dataset;

// internal functions

static void ArHosekSkyModel_CookConfiguration(
        ArHosekSkyModel_Dataset       dataset,
        ArHosekSkyModelConfiguration  config,
        double                        turbidity,
        double                        albedo,
        double                        solar_elevation
        )
{
    double  * elev_matrix;

    int     int_turbidity = (int)turbidity;
    double  turbidity_rem = turbidity - (double)int_turbidity;

    solar_elevation = pow(solar_elevation / (MATH_PI / 2.0), (1.0 / 3.0));

    // alb 0 low turb

    elev_matrix = dataset + ( 9 * 6 * (int_turbidity-1) );


    for( unsigned int i = 0; i < 9; ++i )
    {
        //(1-t).^3* A1 + 3*(1-t).^2.*t * A2 + 3*(1-t) .* t .^ 2 * A3 + t.^3 * A4;
        config[i] =
        (1.0-albedo) * (1.0 - turbidity_rem)
        * ( pow(1.0-solar_elevation, 5.0) * elev_matrix[i]  +
           5.0  * pow(1.0-solar_elevation, 4.0) * solar_elevation * elev_matrix[i+9] +
           10.0*pow(1.0-solar_elevation, 3.0)*pow(solar_elevation, 2.0) * elev_matrix[i+18] +
           10.0*pow(1.0-solar_elevation, 2.0)*pow(solar_elevation, 3.0) * elev_matrix[i+27] +
           5.0*(1.0-solar_elevation)*pow(solar_elevation, 4.0) * elev_matrix[i+36] +
           pow(solar_elevation, 5.0)  * elev_matrix[i+45]);
    }

    // alb 1 low turb
    elev_matrix = dataset + (9*6*10 + 9*6*(int_turbidity-1));
    for(unsigned int i = 0; i < 9; ++i)
    {
        //(1-t).^3* A1 + 3*(1-t).^2.*t * A2 + 3*(1-t) .* t .^ 2 * A3 + t.^3 * A4;
        config[i] +=
        (albedo) * (1.0 - turbidity_rem)
        * ( pow(1.0-solar_elevation, 5.0) * elev_matrix[i]  +
           5.0  * pow(1.0-solar_elevation, 4.0) * solar_elevation * elev_matrix[i+9] +
           10.0*pow(1.0-solar_elevation, 3.0)*pow(solar_elevation, 2.0) * elev_matrix[i+18] +
           10.0*pow(1.0-solar_elevation, 2.0)*pow(solar_elevation, 3.0) * elev_matrix[i+27] +
           5.0*(1.0-solar_elevation)*pow(solar_elevation, 4.0) * elev_matrix[i+36] +
           pow(solar_elevation, 5.0)  * elev_matrix[i+45]);
    }

    if(int_turbidity == 10)
        return;

    // alb 0 high turb
    elev_matrix = dataset + (9*6*(int_turbidity));
    for(unsigned int i = 0; i < 9; ++i)
    {
        //(1-t).^3* A1 + 3*(1-t).^2.*t * A2 + 3*(1-t) .* t .^ 2 * A3 + t.^3 * A4;
        config[i] +=
        (1.0-albedo) * (turbidity_rem)
        * ( pow(1.0-solar_elevation, 5.0) * elev_matrix[i]  +
           5.0  * pow(1.0-solar_elevation, 4.0) * solar_elevation * elev_matrix[i+9] +
           10.0*pow(1.0-solar_elevation, 3.0)*pow(solar_elevation, 2.0) * elev_matrix[i+18] +
           10.0*pow(1.0-solar_elevation, 2.0)*pow(solar_elevation, 3.0) * elev_matrix[i+27] +
           5.0*(1.0-solar_elevation)*pow(solar_elevation, 4.0) * elev_matrix[i+36] +
           pow(solar_elevation, 5.0)  * elev_matrix[i+45]);
    }

    // alb 1 high turb
    elev_matrix = dataset + (9*6*10 + 9*6*(int_turbidity));
    for(unsigned int i = 0; i < 9; ++i)
    {
        //(1-t).^3* A1 + 3*(1-t).^2.*t * A2 + 3*(1-t) .* t .^ 2 * A3 + t.^3 * A4;
        config[i] +=
        (albedo) * (turbidity_rem)
        * ( pow(1.0-solar_elevation, 5.0) * elev_matrix[i]  +
           5.0  * pow(1.0-solar_elevation, 4.0) * solar_elevation * elev_matrix[i+9] +
           10.0*pow(1.0-solar_elevation, 3.0)*pow(solar_elevation, 2.0) * elev_matrix[i+18] +
           10.0*pow(1.0-solar_elevation, 2.0)*pow(solar_elevation, 3.0) * elev_matrix[i+27] +
           5.0*(1.0-solar_elevation)*pow(solar_elevation, 4.0) * elev_matrix[i+36] +
           pow(solar_elevation, 5.0)  * elev_matrix[i+45]);
    }
}

static double ArHosekSkyModel_CookRadianceConfiguration(
        ArHosekSkyModel_Radiance_Dataset  dataset,
        double                            turbidity,
        double                            albedo,
        double                            solar_elevation
        )
{
    double* elev_matrix;

    int int_turbidity = (int)turbidity;
    double turbidity_rem = turbidity - (double)int_turbidity;
    double res;
    solar_elevation = pow(solar_elevation / (MATH_PI / 2.0), (1.0 / 3.0));

    // alb 0 low turb
    elev_matrix = dataset + (6*(int_turbidity-1));
    //(1-t).^3* A1 + 3*(1-t).^2.*t * A2 + 3*(1-t) .* t .^ 2 * A3 + t.^3 * A4;
    res = (1.0-albedo) * (1.0 - turbidity_rem) *
        ( pow(1.0-solar_elevation, 5.0) * elev_matrix[0] +
         5.0*pow(1.0-solar_elevation, 4.0)*solar_elevation * elev_matrix[1] +
         10.0*pow(1.0-solar_elevation, 3.0)*pow(solar_elevation, 2.0) * elev_matrix[2] +
         10.0*pow(1.0-solar_elevation, 2.0)*pow(solar_elevation, 3.0) * elev_matrix[3] +
         5.0*(1.0-solar_elevation)*pow(solar_elevation, 4.0) * elev_matrix[4] +
         pow(solar_elevation, 5.0) * elev_matrix[5]);

    // alb 1 low turb
    elev_matrix = dataset + (6*10 + 6*(int_turbidity-1));
    //(1-t).^3* A1 + 3*(1-t).^2.*t * A2 + 3*(1-t) .* t .^ 2 * A3 + t.^3 * A4;
    res += (albedo) * (1.0 - turbidity_rem) *
        ( pow(1.0-solar_elevation, 5.0) * elev_matrix[0] +
         5.0*pow(1.0-solar_elevation, 4.0)*solar_elevation * elev_matrix[1] +
         10.0*pow(1.0-solar_elevation, 3.0)*pow(solar_elevation, 2.0) * elev_matrix[2] +
         10.0*pow(1.0-solar_elevation, 2.0)*pow(solar_elevation, 3.0) * elev_matrix[3] +
         5.0*(1.0-solar_elevation)*pow(solar_elevation, 4.0) * elev_matrix[4] +
         pow(solar_elevation, 5.0) * elev_matrix[5]);
    if(int_turbidity == 10)
        return res;

    // alb 0 high turb
    elev_matrix = dataset + (6*(int_turbidity));
    //(1-t).^3* A1 + 3*(1-t).^2.*t * A2 + 3*(1-t) .* t .^ 2 * A3 + t.^3 * A4;
    res += (1.0-albedo) * (turbidity_rem) *
        ( pow(1.0-solar_elevation, 5.0) * elev_matrix[0] +
         5.0*pow(1.0-solar_elevation, 4.0)*solar_elevation * elev_matrix[1] +
         10.0*pow(1.0-solar_elevation, 3.0)*pow(solar_elevation, 2.0) * elev_matrix[2] +
         10.0*pow(1.0-solar_elevation, 2.0)*pow(solar_elevation, 3.0) * elev_matrix[3] +
         5.0*(1.0-solar_elevation)*pow(solar_elevation, 4.0) * elev_matrix[4] +
         pow(solar_elevation, 5.0) * elev_matrix[5]);

    // alb 1 high turb
    elev_matrix = dataset + (6*10 + 6*(int_turbidity));
    //(1-t).^3* A1 + 3*(1-t).^2.*t * A2 + 3*(1-t) .* t .^ 2 * A3 + t.^3 * A4;
    res += (albedo) * (turbidity_rem) *
        ( pow(1.0-solar_elevation, 5.0) * elev_matrix[0] +
         5.0*pow(1.0-solar_elevation, 4.0)*solar_elevation * elev_matrix[1] +
         10.0*pow(1.0-solar_elevation, 3.0)*pow(solar_elevation, 2.0) * elev_matrix[2] +
         10.0*pow(1.0-solar_elevation, 2.0)*pow(solar_elevation, 3.0) * elev_matrix[3] +
         5.0*(1.0-solar_elevation)*pow(solar_elevation, 4.0) * elev_matrix[4] +
         pow(solar_elevation, 5.0) * elev_matrix[5]);
    return res;
}

static double ArHosekSkyModel_GetRadianceInternal(
        ArHosekSkyModelConfiguration  configuration,
        double                        theta,
        double                        gamma
        )
{
    const double expM = exp(configuration[4] * gamma);
    const double rayM = cos(gamma)*cos(gamma);
    const double mieM = (1.0 + cos(gamma)*cos(gamma)) / pow((1.0 + configuration[8]*configuration[8] - 2.0*configuration[8]*cos(gamma)), 1.5);
    const double zenith = sqrt(cos(theta));

    return (1.0 + configuration[0] * exp(configuration[1] / (cos(theta) + 0.01))) *
            (configuration[2] + configuration[3] * expM + configuration[5] * rayM + configuration[6] * mieM + configuration[7] * zenith);
}

void arhosekskymodelstate_free(
        ArHosekSkyModelState  * state
        )
{
    free(state);
}

ArHosekSkyModelState  * arhosek_rgb_skymodelstate_alloc_init(
        const double  turbidity,
        const double  albedo,
        const double  elevation
        )
{
    ArHosekSkyModelState* state = ALLOC(ArHosekSkyModelState);

    state->solar_radius = TERRESTRIAL_SOLAR_RADIUS;
    state->turbidity    = turbidity;
    state->albedo       = albedo;
    state->elevation    = elevation;

    for( unsigned int channel = 0; channel < 3; ++channel )
    {
        ArHosekSkyModel_CookConfiguration(
            datasetsRGB[channel],
            state->configs[channel],
            turbidity,
            albedo,
            elevation
            );

        state->radiances[channel] =
        ArHosekSkyModel_CookRadianceConfiguration(
            datasetsRGBRad[channel],
            turbidity,
            albedo,
            elevation
            );
    }

    return state;
}

double arhosek_tristim_skymodel_radiance(
    ArHosekSkyModelState  * state,
    double                  theta,
    double                  gamma,
    int                     channel
    )
{
    return
        ArHosekSkyModel_GetRadianceInternal(
            state->configs[channel],
            theta,
            gamma
            )
        * state->radiances[channel];
}
