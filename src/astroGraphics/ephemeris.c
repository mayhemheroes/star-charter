// ephemeris.c
// 
// -------------------------------------------------
// Copyright 2015-2024 Dominic Ford
//
// This file is part of StarCharter.
//
// StarCharter is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// StarCharter is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with StarCharter.  If not, see <http://www.gnu.org/licenses/>.
// -------------------------------------------------

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include <gsl/gsl_math.h>

#include "astroGraphics/ephemeris.h"
#include "astroGraphics/solarSystem.h"
#include "ephemCalc/jpl.h"
#include "ephemCalc/orbitalElements.h"
#include "coreUtils/asciiDouble.h"
#include "coreUtils/errorReport.h"
#include "mathsTools/julianDate.h"
#include "mathsTools/projection.h"
#include "mathsTools/sphericalTrig.h"
#include "settings/chart_config.h"
#include "vectorGraphics/arrowDraw.h"
#include "vectorGraphics/lineDraw.h"
#include "vectorGraphics/cairo_page.h"

//! convert_body_name_to_int_id - Convert a string name for an object into an integer ID we can pass to ephemCalc
//! \param object_name - String name for a solar system body
//! \return Integer ID

int convert_body_name_to_int_id(const char *object_name) {
    char name[FNAME_LENGTH];
    int output_body_id = -1;

    // Convert the name of the requested objects into numeric object IDs
    strncpy(name, object_name, FNAME_LENGTH);
    name[FNAME_LENGTH - 1] = '\0';
    str_strip(name, name);
    str_lower(name, name);
    if ((strcmp(name, "mercury") == 0) || (strcmp(name, "pmercury") == 0) || (strcmp(name, "p1") == 0))
        output_body_id = 0;
    else if ((strcmp(name, "venus") == 0) || (strcmp(name, "pvenus") == 0) || (strcmp(name, "p2") == 0))
        output_body_id = 1;
    else if ((strcmp(name, "earth") == 0) || (strcmp(name, "pearth") == 0) || (strcmp(name, "p3") == 0))
        output_body_id = 19;
    else if ((strcmp(name, "mars") == 0) || (strcmp(name, "pmars") == 0) || (strcmp(name, "p4") == 0))
        output_body_id = 3;
    else if ((strcmp(name, "jupiter") == 0) || (strcmp(name, "pjupiter") == 0) || (strcmp(name, "p5") == 0))
        output_body_id = 4;
    else if ((strcmp(name, "saturn") == 0) || (strcmp(name, "psaturn") == 0) || (strcmp(name, "p6") == 0))
        output_body_id = 5;
    else if ((strcmp(name, "uranus") == 0) || (strcmp(name, "puranus") == 0) || (strcmp(name, "p7") == 0))
        output_body_id = 6;
    else if ((strcmp(name, "neptune") == 0) || (strcmp(name, "pneptune") == 0) || (strcmp(name, "p8") == 0))
        output_body_id = 7;
    else if ((strcmp(name, "pluto") == 0) || (strcmp(name, "ppluto") == 0) || (strcmp(name, "p9") == 0))
        output_body_id = 8;
    else if ((strcmp(name, "moon") == 0) || (strcmp(name, "pmoon") == 0) || (strcmp(name, "p301") == 0))
        output_body_id = 9;
    else if (strcmp(name, "sun") == 0)
        output_body_id = 10;
    else if (((name[0] == 'a') || (name[0] == 'A')) && valid_float(name + 1, NULL)) {
        // Asteroid, e.g. A1
        output_body_id = 1000000 + (int) get_float(name + 1, NULL);
    } else if (((name[0] == 'c') || (name[0] == 'C')) && valid_float(name + 1, NULL)) {
        // Comet, e.g. C1 (first in datafile)
        output_body_id = 2000000 + (int) get_float(name + 1, NULL);
    } else {
        // Search for comets with matching names

        // Open comet database
        orbitalElements_comets_init();

        // Loop over comets seeing if names match
        int index;
        for (index = 0; index < comet_count; index++) {
            // Fetch comet information
            orbitalElements *item = orbitalElements_comets_fetch(index);

            if ((str_cmp_no_case(name, item->name) == 0) || (str_cmp_no_case(name, item->name2) == 0)) {
                output_body_id = 2000000 + index;
                break;
            }
        }
    }

    if (output_body_id < 0) {
        snprintf(temp_err_string, FNAME_LENGTH, "Unrecognised object name <%s>", name);
        stch_fatal(__FILE__, __LINE__, temp_err_string);
        exit(1);
    }

    return output_body_id;
}

//! ephemerides_fetch - Fetch the ephemeris data for solar system objects to be plotted on a star chart
//! \param s - A <chart_config> structure defining the properties of the star chart to be drawn.

int ephemerides_fetch(ephemeris **ephemeris_data_out, int ephemeris_count,
                      const char (*ephemeris_definitions)[N_TRACES_MAX][FNAME_LENGTH],
                      const char *ephemeris_compute_path) {
    int total_ephemeris_points = 0;

    // Allocate storage for the ephemeris of each solar system object
    (*ephemeris_data_out) = (ephemeris *) malloc(ephemeris_count * sizeof(ephemeris));

    // Loop over each of the solar system objects we are plotting tracks for
    for (int i = 0; i < ephemeris_count; i++) {
        // Fetch the string definition, passed by the user
        // For example: jupiter,2458849.5,2459216.5
        const char *trace_definition = (*ephemeris_definitions)[i];

        // Extract object name, jd_min and jd_max from trace definition string
        const char *in_scan = trace_definition;
        char object_id[FNAME_LENGTH], buffer[FNAME_LENGTH];

        // Read object name into <object_id>
        str_comma_separated_list_scan(&in_scan, object_id);
        snprintf((*ephemeris_data_out)[i].obj_id, FNAME_LENGTH, "%s", object_id);
        const int body_id = convert_body_name_to_int_id(object_id);

        // Read starting Julian day number into (*ephemeris_data_out)[i].jd_start
        str_comma_separated_list_scan(&in_scan, buffer);
        const double jd_start = get_float(buffer, NULL);
        (*ephemeris_data_out)[i].jd_start = jd_start;

        // Read ending Julian day number into (*ephemeris_data_out)[i].jd_end
        str_comma_separated_list_scan(&in_scan, buffer);
        const double jd_end = get_float(buffer, NULL);
        (*ephemeris_data_out)[i].jd_end = jd_end;

        // Sample planet's movement every 12 hours
        const double jd_step = 0.5;
        (*ephemeris_data_out)[i].jd_step = jd_step;

        const double ephemeris_duration = jd_end - jd_start; // days

        // Generous estimate of how many lines we expect ephemerisCompute to return
        const int point_count = (int) (2 + ephemeris_duration / jd_step);
        (*ephemeris_data_out)[i].point_count = point_count;

        // Keep track of the brightest magnitude and largest angular size of the object
        (*ephemeris_data_out)[i].brightest_magnitude = 999;
        (*ephemeris_data_out)[i].minimum_phase = 1;
        (*ephemeris_data_out)[i].maximum_angular_size = 0;

        // Allocate data to hold the ephemeris
        (*ephemeris_data_out)[i].data = (ephemeris_point *) malloc(
                (*ephemeris_data_out)[i].point_count * sizeof(ephemeris_point)
        );

        // Loop over points in the ephemeris
        for (int line_counter = 0; line_counter < point_count; line_counter++) {
            const double jd = jd_start + line_counter * jd_step;
            if (jd > jd_end) {
                // Throw an error if we got no data
                if (line_counter == 0) {
                    stch_fatal(__FILE__, __LINE__, "ephemeris-compute-de430 returned no data");
                    exit(1);
                }

                // Record how many lines of data were returned from ephemeris-compute-de430
                (*ephemeris_data_out)[i].point_count = line_counter;

                // Ended
                break;
            }

            double x, y, z, ra, dec, magnitude, phase, angular_size, physical_size, albedo;
            double sun_distance, earth_distance, theta_oes, theta_eso;
            double ecliptic_longitude_j2000, ecliptic_latitude_j2000, longitude_diff;

            jpl_computeEphemeris(body_id, jd, &x, &y, &z,
                                 &ra, &dec, &magnitude, &phase, &angular_size, &physical_size,
                                 &albedo, &sun_distance, &earth_distance, &theta_oes, &theta_eso,
                                 &ecliptic_longitude_j2000, &ecliptic_latitude_j2000,
                                 &longitude_diff);

            // Store this data point into (*ephemeris_data_out)
            (*ephemeris_data_out)[i].data[line_counter].jd = jd;
            (*ephemeris_data_out)[i].data[line_counter].ra = ra;
            (*ephemeris_data_out)[i].data[line_counter].dec = dec;
            (*ephemeris_data_out)[i].data[line_counter].mag = magnitude;
            (*ephemeris_data_out)[i].data[line_counter].phase = phase;
            (*ephemeris_data_out)[i].data[line_counter].angular_size = angular_size;
            (*ephemeris_data_out)[i].data[line_counter].text_label = NULL;
            (*ephemeris_data_out)[i].data[line_counter].sub_month_label = 0;

            // Keep track of maximum values
            if (magnitude < (*ephemeris_data_out)[i].brightest_magnitude) {
                (*ephemeris_data_out)[i].brightest_magnitude = magnitude;
            }

            if (phase < (*ephemeris_data_out)[i].minimum_phase) {
                (*ephemeris_data_out)[i].minimum_phase = phase;
            }

            if (angular_size > (*ephemeris_data_out)[i].maximum_angular_size) {
                (*ephemeris_data_out)[i].maximum_angular_size = angular_size;
            }

        }

        // Keep tally of the sum total number of points in all ephemerides
        total_ephemeris_points += (*ephemeris_data_out)[i].point_count;
    }

    // Return total number of points in all ephemerides
    return total_ephemeris_points;
}


//! ephemerides_free - Free memory allocated to store ephemeride data for solar system objects
//! \param s - A <chart_config> structure defining the properties of the star chart to be drawn.

void ephemerides_free(chart_config *s) {
    int i;
    for (i = 0; i < s->ephemeris_final_count; i++) {
        if (s->ephemeris_data[i].data != NULL) {
            free(s->ephemeris_data[i].data);
        }
    }
    free(s->ephemeris_data);
}

//! ephemerides_autoscale_plot - Automatically scale plot to contain all of the computed ephemeris tracks
//! \param s - A <chart_config> structure defining the properties of the star chart to be drawn.

void ephemerides_autoscale_plot(chart_config *s, const int total_ephemeris_points) {
    int i, j, k;

    // Track the sky coverage of each ephemeris in RA and Dec
    // Create a coarse grid of RA and Declination where we set Boolean flags for whether the solar system object passes
    // through each cell.
    int ra_bins = 24 * 16;
    int dec_bins = 18 * 16;
    int *ra_usage = (int *) malloc(ra_bins * sizeof(int));
    int *dec_usage = (int *) malloc(dec_bins * sizeof(int));

    // Zero map of sky coverage
    for (i = 0; i < ra_bins; i++) ra_usage[i] = 0;
    for (i = 0; i < dec_bins; i++) dec_usage[i] = 0;

    // For the purposes of working out minimal sky area encompassing all ephemerides, we concatenate all ephemerides
    // into a single big array
    double *ra_list = (double *) malloc(total_ephemeris_points * sizeof(double));
    double *dec_list = (double *) malloc(total_ephemeris_points * sizeof(double));

    // Loop over all the ephemeris tracks we have computed, and populate the grids <ra_usage> and <dec_usage> with all
    // the cells that any of the moving objects visited
    for (i = j = 0; i < s->ephemeris_final_count; i++)
        for (k = 0; k < s->ephemeris_data[i].point_count; j++, k++) {
            // Populate the big list of all ephemeris data points
            ra_list[j] = s->ephemeris_data[i].data[k].ra;
            dec_list[j] = s->ephemeris_data[i].data[k].dec;

            // Work out which grid cell this ephemeris point falls within in the grids <ra_usage> and <dec_usage>
            int ra_bin = (int) (ra_list[j] / (2 * M_PI) * ra_bins);
            int dec_bin = (int) ((dec_list[j] + (M_PI / 2)) / M_PI * dec_bins);

            // Mark this point on the map of usage of RA and Dec
            ra_usage[ra_bin] = 1;
            dec_usage[dec_bin] = 1;
            // printf("pt %4d: %.3f %.3f\n", j, ra_list[j] * 12 / M_PI, dec_list[j] * 180 / M_PI);
        }

    // Work out centroid on the sky of all ephemeris data points
    double ra_centroid, dec_centroid;
    find_mean_position(&ra_centroid, &dec_centroid, ra_list, dec_list, total_ephemeris_points);
    // printf("centroid: %.3f %.3f\n", ra_centroid * 12 / M_PI, dec_centroid * 180 / M_PI);

    // Work out which bin the centroid falls within
    int ra_centre_bin = (int) (ra_centroid / (2 * M_PI) * ra_bins);
    // int dec_centre_bin = (int) ((dec_centroid + (M_PI / 2)) / M_PI * dec_bins);

    // Find the RA opposite in the sky to the centroid of all ephemeris
    double ra_anti_centre = ra_centroid + M_PI;
    //double dec_anti_centre = -dec_centroid;

    // Work out which bin of RA/Dec coverage the opposite point falls within
    int ra_anti_centre_bin = (int) (ra_anti_centre / (2 * M_PI) * ra_bins);
    //int dec_anti_centre_bin = (int) ((dec_anti_centre + (M_PI / 2)) / M_PI * dec_bins);

    // Make sure that this falls within the range of the array <ra_usage> (since RA wraps around from 0 to 2pi).
    while (ra_anti_centre_bin < 0) ra_anti_centre_bin += ra_bins;
    while (ra_anti_centre_bin >= ra_bins) ra_anti_centre_bin -= ra_bins;

    // Peel back sky coverage east and west from anti-centre until an ephemeris is reached

    // Find minimum RA used, wrapping around the RA=24h
    int ra_bin_min = ra_anti_centre_bin + 1;
    while (!ra_usage[ra_bin_min]) {
        // If we reach the centroid of the chart, something has gone wrong
        if (ra_bin_min == ra_centre_bin) {
            s->ephemeris_autoscale = 0;
            break;
        }
        ra_bin_min = (ra_bin_min + 1) % ra_bins;
    }

    // Find maximum RA used, wrapping around the RA=24h
    int ra_bin_max = ra_anti_centre_bin;
    while (!ra_usage[ra_bin_max]) {
        // If we reach the centroid of the chart, something has gone wrong
        if (ra_bin_max == ra_centre_bin) {
            s->ephemeris_autoscale = 0;
            break;
        }
        ra_bin_max = (ra_bin_max + ra_bins - 1) % ra_bins;
    }

    // Find southernmost declination used
    int dec_bin_min = 0;
    while (!dec_usage[dec_bin_min]) {
        // If we reach the centroid of the chart, something has gone wrong
        if (dec_bin_min == dec_bins - 1) {
            s->ephemeris_autoscale = 0;
            break;
        }
        dec_bin_min++;
    }

    // Find northernmost declination used
    int dec_bin_max = dec_bins - 1;
    while (!dec_usage[dec_bin_max]) {
        // If we reach the centroid of the chart, something has gone wrong
        if (dec_bin_max == 0) {
            s->ephemeris_autoscale = 0;
            break;
        }
        dec_bin_max--;
    }

    // We have now fully determined the maximum limits of all the ephemerides, north, south, east and west.

    // Convert RA and Dec of the bounding box of the star chart from bin numbers back into angles
    double ra_min = ra_bin_min * 24. / ra_bins;  // hours
    double ra_max = (ra_bin_max + 1) * 24. / ra_bins;  // hours ; ra_bin_max points to the last occupied bin
    double dec_min = dec_bin_min * 180. / dec_bins - 90; // degrees
    double dec_max = (dec_bin_max + 1) * 180. / dec_bins - 90; // degrees

    // Make sure that angles fall within range
    while (ra_max <= ra_min) ra_max += 24;
    while (ra_max > ra_min + 24) ra_max -= 24;
    // printf("RA %.4f %.4f; Dec %.4f %.4f\n", ra_min, ra_max, dec_min, dec_max);

    // Work out maximum angular size of the star chart we need
    double angular_width_base = gsl_max((ra_max - ra_min) * 180 / 12, dec_max - dec_min) * 1.15;

    // If star chart covers almost the whole sky, it may as well cover the entire sky
    if (angular_width_base > 350) angular_width_base = 360;

    // Report sky coverage
    if (DEBUG) {
        char message[FNAME_LENGTH];
        snprintf(message, FNAME_LENGTH, "  RA  range: %.1fh to %.1fh", ra_min, ra_max);
        stch_log(message);
        snprintf(message, FNAME_LENGTH, "  Dec range: %.1fd to %.1fd", dec_min, dec_max);
        stch_log(message);
        snprintf(message, FNAME_LENGTH, "  Ang width: %.1f deg", angular_width_base);
        stch_log(message);
    }

    // If plot is auto-scaling, set coordinates for the centre and the angular extent
    if (s->ephemeris_autoscale) {
        // Set magnitude limits
        if (s->mag_min_automatic) {
            for (i = 0; i < s->ephemeris_final_count; i++) {
                // Show stars two magnitudes fainter than target
                const double magnitude_margin = 2;

                // Limiting magnitude for stars
                s->mag_min = gsl_max(
                        s->mag_min,
                        ceil((s->ephemeris_data[i].brightest_magnitude + magnitude_margin) / s->mag_step) * s->mag_step
                );

                // Limiting magnitude for deep sky objects
                s->dso_mag_min = gsl_max(4, s->mag_min + 1);
            }

            // If we have very few stars, then extend magnitude limit to fainter stars
            s->minimum_star_count = s->maximum_star_count / 8;
        }

        // The coordinates of the centre of the star chart
        s->ra0 = (ra_min + ra_max) / 2;
        s->dec0 = (dec_min + dec_max) / 2;

        // Make sure that RA is within range
        while (s->ra0 < 0) s->ra0 += 24;
        while (s->ra0 >= 24) s->ra0 -= 24;

        // Decide what labels to show, based on how wide the area of sky we're showing
        s->star_flamsteed_labels = (int) (angular_width_base < 30);
        s->star_bayer_labels = (int) (angular_width_base < 70);
        s->constellation_names = 1;

        if (angular_width_base < 8) {
            s->star_catalogue = SW_CAT_HIP;
            s->star_catalogue_numbers = 1;
            s->maximum_star_label_count = 20;
        }

        // Set an appropriate projection
        if (angular_width_base > 110) {
            // Charts wider than 110 degrees should use a rectangular projection, not a gnomonic projection
            s->projection = SW_PROJECTION_FLAT;
            s->angular_width = angular_width_base;

            // Plots which cover the whole sky need to be really big...
            s->width *= 1.6;
            s->font_size *= 0.95;
            s->mag_min = gsl_min(s->mag_min, 6);
            s->maximum_star_label_count = 25;
            s->dso_names = 0;

            // Normally use an aspect ratio of 0.5, but if RA span is large and Dec span small, go wide and thin
            s->aspect = gsl_min(0.5, fabs(dec_max - dec_min) / (fabs(ra_max - ra_min) * 180 / 12) * 1.8);

            // Deal with tall narrow finder charts
            if (fabs(dec_max - dec_min) / (fabs(ra_max - ra_min) * 180 / 12) > 0.5) {
                s->aspect = 1;
                s->width *= 0.7;
            }

            // Make sure that plot does not go outside the declination range -90 to 90
            double ang_height = angular_width_base * s->aspect;
            s->dec0 = gsl_max(s->dec0, -89 + ang_height / 2);
            s->dec0 = gsl_min(s->dec0, 89 - ang_height / 2);

        } else {
            // Charts which cover less than 110 degrees should use a stereographic projection
            s->projection = SW_PROJECTION_STEREOGRAPHIC;

            // Pick an attractive aspect ratio for this chart
            s->aspect = ceil(fabs(dec_max - dec_min) / (fabs(ra_max - ra_min) * 180 / 12) * 10.) / 10.;
            if (s->aspect < 0.6) s->aspect = 0.6;
            if (s->aspect > 1.5) s->aspect = 1.5;

            // Fix angular width to take account of the aspect ratio of the plotting area
            double angular_width = gsl_max((ra_max - ra_min) * 180 / 12, (dec_max - dec_min) / s->aspect) * 1.1;
            if (angular_width > 350) angular_width = 360;
            s->angular_width = angular_width;
        }
    }

    // Free up storage
    free(ra_list);
    free(dec_list);
    free(ra_usage);
    free(dec_usage);
}

//! ephemerides_add_manual_text_labels - Add text labels at manually-specified points along the ephemeris tracks
//! (when the <ephemeris_epochs> setting has been supplied).
//! \param s - A <chart_config> structure defining the properties of the star chart to be drawn.

void ephemerides_add_manual_text_labels(chart_config *s) {
    // Loop over all the ephemeris tracks we have computed, and add text labels
    for (int i = 0; i < s->ephemeris_final_count; i++) {
        // Loop over points within the ephemeris
        for (int line_counter = 0; line_counter < s->ephemeris_data[i].point_count; line_counter++) {
            const int is_first_point = (line_counter < 1);
            const int is_last_point = (line_counter >= s->ephemeris_data[i].point_count - 1);

            const double jd_pt = s->ephemeris_data[i].data[line_counter].jd;

            // Loop over manually specified epochs to label, and see if any fit this time point
            int label_index = -1;
            for (int j = 0; j < s->ephemeris_epochs_final_count; j++) {
                const double jd_requested = get_float(s->ephemeris_epochs[j], NULL);
                const double offset_this = fabs(jd_requested - jd_pt);

                // Only consider putting this label here is the JD matches to within 24 hours
                if (offset_this < 1) {
                    const double jd_previous = is_first_point ? 999 : (s->ephemeris_data[i].data[line_counter - 1].jd);
                    const double jd_next = is_last_point ? 999 : (s->ephemeris_data[i].data[line_counter + 1].jd);

                    const double offset_previous = fabs(jd_requested - jd_previous);
                    const double offset_next = fabs(jd_requested - jd_next);

                    // In the middle of an ephemeris, labels attach to the closest ephemeris time point
                    if ((offset_this < offset_previous) && (offset_this <= offset_next)) {
                        label_index = j;
                        break;
                    }
                }
            }

            if (label_index < 0) {
                // If we didn't find a label for this ephemeris point, set it to NULL
                s->ephemeris_data[i].data[line_counter].text_label = NULL;
                s->ephemeris_data[i].data[line_counter].sub_month_label = 0;
            } else {
                // Attach requested label to this ephemeris point
                s->ephemeris_data[i].data[line_counter].text_label =
                        string_make_permanent(s->ephemeris_epoch_labels[label_index]);
                s->ephemeris_data[i].data[line_counter].sub_month_label = 0;
            }
        }
    }
}

//! ephemerides_add_automatic_text_labels - Add text labels at automatically-determined spacing along the ephemeris
//! tracks (when the <ephemeris_epochs> setting is not supplied).
//! \param s - A <chart_config> structure defining the properties of the star chart to be drawn.

void ephemerides_add_automatic_text_labels(chart_config *s) {
    // Loop over all the ephemeris tracks we have computed, and add text labels
    for (int i = 0; i < s->ephemeris_final_count; i++) {
        const double ephemeris_duration = s->ephemeris_data[i].jd_end - s->ephemeris_data[i].jd_start; // days

        const char *previous_label = "";
        int previous_day_of_month = -1;
        for (int line_counter = 0; line_counter < s->ephemeris_data[i].point_count; line_counter++) {
            const double jd = s->ephemeris_data[i].data[line_counter].jd;

            // Work out angular speed of object, to decide how often to write labels
            double angular_speed = 1e6; // radians per day
            if (line_counter > 0) {
                angular_speed = angDist_RADec(s->ephemeris_data[i].data[line_counter - 1].ra,
                                              s->ephemeris_data[i].data[line_counter - 1].dec,
                                              s->ephemeris_data[i].data[line_counter].ra,
                                              s->ephemeris_data[i].data[line_counter].dec
                ) / s->ephemeris_data[i].jd_step;
            }

            // Convert from radians/day to cm/day
            const double cm_per_radian = s->width / s->angular_width;
            const double angular_speed_cm_day = angular_speed * cm_per_radian;
            //printf("%10.6f / %10.6f / %10.6f\n", angular_speed, cm_per_radian, angular_speed_cm_day);

            // Extract calendar date components for this ephemeris data point
            int year, month, day, hour, minute, status = 0;
            double second;
            inv_julian_day(jd, &year, &month, &day, &hour, &minute, &second, &status, temp_err_string);

            // Abort on error
            if (status) {
                stch_error(temp_err_string);
                continue;
            }

            // Create a text label for this point on the ephemeris track
            char label[FNAME_LENGTH] = "";
            int sub_month_label = 0;

            if (day != previous_day_of_month) {
                if (previous_label[0] == '\0') {
                    if (day == 31) {
                        // Don't label the last day of December at the beginning of year-long ephemerides
                    } else {
                        // For the first label on an ephemeris, include the year, e.g. 1 Jan 2022
                        snprintf(label, FNAME_LENGTH, "%d %.3s %d",
                                 day, get_month_name(month), year);
                    }
                } else if (day > 1) {
                    if ((ephemeris_duration < 15) || (angular_speed_cm_day > 0.025)) {
                        // For ephemerides spanning less than a month, label every day
                        snprintf(label, FNAME_LENGTH, "%d", day); // Show day of month only
                        sub_month_label = 1;
                    } else if (((day % 7) == 0) && ((ephemeris_duration < 70) || (angular_speed_cm_day > 0.025 / 7))) {
                        // Within each month, place labels at weekly intervals
                        snprintf(label, FNAME_LENGTH, "%d", day); // Show day of month only
                        sub_month_label = 1;
                    }
                } else if (month == 1) {
                    // In January, and in the first label on an ephemeris, include the year
                    snprintf(label, FNAME_LENGTH, "%.3s %d", get_month_name(month), year); // e.g. Jan 2022
                } else {
                    // Omit the year in subsequent new months within the same year
                    snprintf(label, FNAME_LENGTH, "%.3s", get_month_name(month)); // e.g. Aug
                }
            }

            // Decide whether to show this label. Do so if we've just entered a new month.
            if ((label[0] != '\0') && (strncmp(label, previous_label, 3) != 0)) {
                s->ephemeris_data[i].data[line_counter].text_label = string_make_permanent(label);
                s->ephemeris_data[i].data[line_counter].sub_month_label = sub_month_label;

                // Keep track of the previous label we have shown on this track, which we use to decide when to
                // display a next label
                previous_label = s->ephemeris_data[i].data[line_counter].text_label;
            }

            previous_day_of_month = day;
        }
    }
}

//! plot_ephemeris - Plot an ephemeris for a solar system object.
//! \param s - A <chart_config> structure defining the properties of the star chart to be drawn.
//! \param ld - A <line_drawer> structure used to draw lines on a cairo surface.
//! \param page - A <cairo_page> structure defining the cairo drawing context.
//! \param trace_num - The number of the ephemeris to draw (an index within <s->ephemeris_definitions>).

void plot_ephemeris(chart_config *s, line_drawer *ld, cairo_page *page, int trace_num) {
    int i;
    double last_x = 0, last_y = 0, initial_theta = 0.0;

    // Pointer to ephemeris data
    const ephemeris *e = &s->ephemeris_data[trace_num];

    // Work out what colour to use
    const int ephemeris_colour_index = trace_num % s->ephemeris_col_final_count;
    const colour colour_final = s->ephemeris_col[ephemeris_colour_index];
    const colour colour_label_final = s->ephemeris_label_col[ephemeris_colour_index];

    const int solar_system_colour_index = trace_num % s->solar_system_colour_final_count;
    const colour colour_planet_final = s->solar_system_colour[solar_system_colour_index];

    // Draw ephemeris line, if needed
    if ((s->ephemeris_style == SW_EPHEMERIS_TRACK) || (s->ephemeris_style == SW_EPHEMERIS_SIDE_BY_SIDE_WITH_TRACK)) {
        // Set line width
        const double line_width = 2;
        cairo_set_line_width(s->cairo_draw, line_width * s->line_width_base);

        // Set line colour
        ld_pen_up(ld, GSL_NAN, GSL_NAN, NULL, 1);
        cairo_set_source_rgb(s->cairo_draw, colour_final.red, colour_final.grn, colour_final.blu);
        ld_label(ld, NULL, 1, 1);

        // Loop over the points in the ephemeris, and draw a line across the star chart
        for (i = 0; i < e->point_count; i++) {
            double x, y;

            // Work out the coordinates of each ephemeris data point on the plotting canvas
            plane_project(&x, &y, s, e->data[i].ra, e->data[i].dec);
            if ((x < s->x_min) || (x > s->x_max) || (y < s->y_min) || (y > s->y_max)) continue;

            // Add this point to the line we are tracing
            ld_point(ld, x, y, NULL);

            // Store initial direction of ephemeris track to use later when drawing ticks perpendicular to it
            if (i == 2) initial_theta = atan2(y - last_y, x - last_x);
            last_x = x;
            last_y = y;
        }

        // We have finished tracing ephemeris line, so lift the pen
        ld_pen_up(ld, GSL_NAN, GSL_NAN, NULL, 1);
    }

    // Draw tick marks to indicate notable points along the path of the object
    if (s->ephemeris_style == SW_EPHEMERIS_TRACK) {
        int is_first_label = 1;

        for (i = 0; i < e->point_count; i++) {
            double x, y, theta;

            // Work out how long this tick mark should be; major time points get longer ticks
            const double physical_tick_len = e->data[i].sub_month_label ? 0.12 : 0.2; // cm
            const double line_width = e->data[i].sub_month_label ? 0.8 : 2;
            const double graph_coords_tick_len = physical_tick_len * s->wlin / s->width;

            cairo_set_line_width(s->cairo_draw, line_width * s->line_width_base);

            // Work out coordinates of this tick mark on the plotting canvas
            plane_project(&x, &y, s, e->data[i].ra, e->data[i].dec);

            // Work out direction of ephemeris track
            if (i < 2) theta = initial_theta;
            else theta = atan2(y - last_y, x - last_x);

            // This happens when ephemeris track is the same twice running; deal gracefully with it
            if (!gsl_finite(theta)) theta = 0.0;

            last_x = x;
            last_y = y;

            // Add point to label exclusion region so that labels don't collide with it
            page->exclusion_regions[page->exclusion_region_counter].x_min = x - graph_coords_tick_len * 0.1;
            page->exclusion_regions[page->exclusion_region_counter].x_max = x + graph_coords_tick_len * 0.1;
            page->exclusion_regions[page->exclusion_region_counter].y_min = y - graph_coords_tick_len * 0.1;
            page->exclusion_regions[page->exclusion_region_counter].y_max = y + graph_coords_tick_len * 0.1;
            page->exclusion_region_counter++;

            // Check for buffer overrun
            if (page->exclusion_region_counter >= MAX_EXCLUSION_REGIONS) {
                stch_fatal(__FILE__, __LINE__, "Exceeded maximum label exclusion regions");
            }

            // Make tick mark
            if (e->data[i].text_label != NULL) {
                int h_align, v_align;
                const double theta_deg = theta * 180 / M_PI;

                // Reject this tick mark if it's off the side of the star chart
                if ((x < s->x_min) || (x > s->x_max) || (y < s->y_min) || (y > s->y_max)) continue;

                // Add tick mark to label exclusion region so that labels don't collide with it
                page->exclusion_regions[page->exclusion_region_counter].x_min = x - graph_coords_tick_len * 0.4;
                page->exclusion_regions[page->exclusion_region_counter].x_max = x + graph_coords_tick_len * 0.4;
                page->exclusion_regions[page->exclusion_region_counter].y_min = y - graph_coords_tick_len * 0.4;
                page->exclusion_regions[page->exclusion_region_counter].y_max = y + graph_coords_tick_len * 0.4;
                page->exclusion_region_counter++;

                // Draw tick mark
                ld_pen_up(ld, GSL_NAN, GSL_NAN, NULL, 1);
                ld_label(ld, NULL, 1, 1);
                ld_point(ld, x + graph_coords_tick_len * sin(theta), y - graph_coords_tick_len * cos(theta), NULL);
                ld_point(ld, x - graph_coords_tick_len * sin(theta), y + graph_coords_tick_len * cos(theta), NULL);
                ld_pen_up(ld, GSL_NAN, GSL_NAN, NULL, 1);

                // Work out horizontal and vertical alignment of this label, based on the direction the ephemeris is
                // travelling in. From this, decide how the tick label text should be aligned relative to the end of the
                // tick marker.
                if (theta_deg < -135 - 22.5) {
                    h_align = 0; // centre
                    v_align = -1; // middle
                } else if (theta_deg < -90 - 22.5) {
                    h_align = 1; // right
                    v_align = -1;
                } else if (theta_deg < -45 - 22.5) {
                    h_align = 1;
                    v_align = 0;
                } else if (theta_deg < -22.5) {
                    h_align = 1;
                    v_align = 1;
                } else if (theta_deg < +22.5) {
                    h_align = 0;
                    v_align = 1;
                } else if (theta_deg < 45 + 22.5) {
                    h_align = -1; // left
                    v_align = 1;
                } else if (theta_deg < 90 + 22.5) {
                    h_align = -1;
                    v_align = 0;
                } else if (theta_deg < 135 + 22.5) {
                    h_align = -1;
                    v_align = -1;
                } else {
                    h_align = 0;
                    v_align = -1;
                }

                // Offer the renderer four possible positions where the tick text can be placed

                // Two points, one on either end of the tick marker
                const double label_gap_1 = 1.5;
                const double xp_a = x + label_gap_1 * graph_coords_tick_len * sin(theta);
                const double yp_a = y - label_gap_1 * graph_coords_tick_len * cos(theta);
                const double xp_b = x - label_gap_1 * graph_coords_tick_len * sin(theta);
                const double yp_b = y + label_gap_1 * graph_coords_tick_len * cos(theta);

                // Two further points, also on either end of the tick marker, but this time further out
                const double label_gap_2 = 1.85;
                const double xp_c = x + label_gap_2 * graph_coords_tick_len * sin(theta);
                const double yp_c = y - label_gap_2 * graph_coords_tick_len * cos(theta);
                const double xp_d = x - label_gap_2 * graph_coords_tick_len * sin(theta);
                const double yp_d = y + label_gap_2 * graph_coords_tick_len * cos(theta);

                // Prioritise labels at start of years and quarters
                double priority;

                if (s->must_show_all_ephemeris_labels || is_first_label) {
                    priority = -1;
                } else {
                    priority = 0.0123 + (1e-12 * i) - (4e-6 * (!e->data[i].sub_month_label));
                }

                // Write text label
                const double font_size = e->data[i].sub_month_label ? 1.6 : 1.8;
                const double extra_margin = e->data[i].sub_month_label ? 2 : 0;
                chart_label_buffer(page, s, colour_label_final, e->data[i].text_label,
                                   (label_position[4]) {
                                           {xp_a, yp_a, 0, 0, 0, h_align,  v_align},
                                           {xp_b, yp_b, 0, 0, 0, -h_align, -v_align},
                                           {xp_c, yp_c, 0, 0, 0, h_align,  v_align},
                                           {xp_d, yp_d, 0, 0, 0, -h_align, -v_align}
                                   }, 4,
                                   0, 1, font_size, 1, 0,
                                   extra_margin, priority);

                // We have now rendered first label
                is_first_label = 0;
            }
        }
    }

    // Draw multiple object images side-by-side, if needed
    if (
            (s->ephemeris_style == SW_EPHEMERIS_SIDE_BY_SIDE) ||
            (s->ephemeris_style == SW_EPHEMERIS_SIDE_BY_SIDE_WITH_TRACK) ||
            (s->ephemeris_style == SW_EPHEMERIS_SIDE_BY_SIDE_WITH_ARROW)
            ) {
        // Test if we are plotting the Moon
        const int is_moon = (strcmp(e->obj_id, "P301") == 0);

        // Loop over the points in the ephemeris, and draw a line across the star chart
        for (i = 0; i < e->point_count; i++) {
            double x, y;

            // Work out the coordinates of each ephemeris data point on the plotting canvas
            plane_project(&x, &y, s, e->data[i].ra, e->data[i].dec);
            if ((x < s->x_min) || (x > s->x_max) || (y < s->y_min) || (y > s->y_max)) continue;

            // Only draw a planet representations for data points with associated text labels
            if (e->data[i].text_label != NULL) {
                // Draw object
                if (is_moon && s->solar_system_show_moon_phase) {
                    // Show Moon with representation of phase
                    draw_moon(s, page, colour_label_final, x, y, e->data[i].ra, e->data[i].dec,
                              e->data[i].jd, e->data[i].text_label);
                } else {
                    // Draw a circular splodge on the star chart
                    draw_solar_system_object(s, page, colour_planet_final, colour_label_final,
                                             e->data[i].mag, x, y, e->data[i].text_label);
                }
            }
        }
    }

    // Overlay an arrow, if requested
    if (s->ephemeris_style == SW_EPHEMERIS_SIDE_BY_SIDE_WITH_ARROW) {
        const double line_width = 2;

        // Disabled pass 2; currently only draw black arrow
        for (int pass = 0; pass < 2; pass++) {
            // Set line colour and line width
            if (pass == 0) {
                ld_pen_up(ld, GSL_NAN, GSL_NAN, NULL, 1);
                cairo_set_line_width(s->cairo_draw, line_width * s->line_width_base * 1.2);
                cairo_set_source_rgb(s->cairo_draw, 0, 0, 0);
            } else {
                ld_pen_up(ld, GSL_NAN, GSL_NAN, NULL, 1);
                cairo_set_line_width(s->cairo_draw, line_width * s->line_width_base);
                cairo_set_source_rgb(s->cairo_draw, colour_final.red, colour_final.grn, colour_final.blu);
            }
            ld_label(ld, NULL, 1, 1);

            // Loop over the points in the ephemeris, and draw a line across the star chart
            for (i = 0; i < e->point_count; i++) {
                double x, y;

                // Work out the coordinates of each ephemeris data point on the plotting canvas
                plane_project(&x, &y, s, e->data[i].ra, e->data[i].dec);
                if ((x < s->x_min) || (x > s->x_max) || (y < s->y_min) || (y > s->y_max)) continue;

                // Add this point to the line we are tracing
                ld_point(ld, x, y, NULL);

                // Store initial direction of ephemeris track to use later when drawing ticks perpendicular to it
                if (i == 2) initial_theta = atan2(y - last_y, x - last_x);
                last_x = x;
                last_y = y;
            }

            // We have finished tracing ephemeris line, so lift the pen
            ld_pen_up(ld, GSL_NAN, GSL_NAN, NULL, 1);

            // Draw final arrow head
            if (e->point_count > 2) {
                const int i0 = e->point_count - 3;
                const int i1 = e->point_count - 1;
                double x0, y0, x1, y1;

                // Work out the coordinates of each ephemeris data point on the plotting canvas
                plane_project(&x0, &y0, s, e->data[i0].ra, e->data[i0].dec);
                if ((x0 < s->x_min) || (x0 > s->x_max) || (y0 < s->y_min) || (y0 > s->y_max)) continue;

                plane_project(&x1, &y1, s, e->data[i1].ra, e->data[i1].dec);
                if ((x1 < s->x_min) || (x1 > s->x_max) || (y1 < s->y_min) || (y1 > s->y_max)) continue;

                // Convert to canvas coordinates
                double x0_canvas, y0_canvas, x1_canvas, y1_canvas;
                fetch_canvas_coordinates(&x0_canvas, &y0_canvas, x0, y0, s);
                fetch_canvas_coordinates(&x1_canvas, &y1_canvas, x1, y1, s);

                // Draw arrow
                draw_arrow(s, line_width, 0, 1,
                           x0_canvas, y0_canvas, x1_canvas, y1_canvas);
            }
        }
    }
}

//! draw_ephemeris_table - Draw a table of the brightness of the object whose ephemeris we have drawn
//! \param s - A <chart_config> structure defining the properties of the star chart to be drawn.
//! \param legend_y_pos - The vertical pixel position of the top of the next legend to go under the star chart.
//! \param draw_output - Boolean indicating whether we draw output, or simply measure the output
//! \param width_out - Output the width of the legend we produced

double draw_ephemeris_table(chart_config *s, double legend_y_pos, int draw_output, double *width_out) {
    char text_buffer[FNAME_LENGTH];

    // If requested, output the width of the table we produced
    if (width_out != NULL) *width_out = 0;

    // Loop over objects in turn
    for (int i = 0; i < s->ephemeris_final_count; i++) {
        // Pointer to ephemeris data
        const ephemeris *e = &s->ephemeris_data[i];

        // Duration of ephemeris, days
        const double ephemeris_duration = s->ephemeris_data[i].jd_end - s->ephemeris_data[i].jd_start;

        // Column widths (in cm) -- Date, Magnitude, Phase, Angular size
        double col_widths[] = {3.5, 1.5, 1.5, 2.5, 0, 0};

        // Hide phase column if object always shows full phase
        if (e->minimum_phase > 0.96) col_widths[2] = 0;

        // Decide whether to express angular size in arcseconds or arcminutes
        char angular_size_unit = '"';
        if (e->maximum_angular_size > 120) angular_size_unit = '\'';
        if (e->maximum_angular_size < 1) col_widths[3] = 0;

        // Work out physical dimensions of table
        // The text offset
        const double margin_h = 0.15;
        const double margin_v = 0.1;

        // The width of the table
        const double w_item = col_widths[0] + col_widths[1] + col_widths[2] + col_widths[3];

        if (width_out != NULL) *width_out = gsl_max(*width_out, w_item);

        // The horizontal position of the left edge of the legend
        const double x0 = s->canvas_offset_x + s->width - w_item;

        // The horizontal position of the right edge of the legend
        const double x2 = s->canvas_offset_x + s->width;

        // The top edge of the legend
        const double y0 = legend_y_pos;

        // Reset font and line width
        double font_size = 3.6 * s->mm * s->font_size / s->cm; // cm
        double line_height = font_size * 1.3;
        legend_y_pos += line_height;
        double y = legend_y_pos - margin_v;

        if (draw_output) {
            cairo_select_font_face(s->cairo_draw, s->font_family, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
            cairo_set_font_size(s->cairo_draw, font_size * s->cm);
            cairo_set_line_width(s->cairo_draw, 2 * s->line_width_base);
            cairo_set_source_rgb(s->cairo_draw, 0, 0, 0);

            cairo_move_to(s->cairo_draw, x0 * s->cm, (legend_y_pos - line_height) * s->cm);
            cairo_line_to(s->cairo_draw, x2 * s->cm, (legend_y_pos - line_height) * s->cm);
            cairo_move_to(s->cairo_draw, x0 * s->cm, legend_y_pos * s->cm);
            cairo_line_to(s->cairo_draw, x2 * s->cm, legend_y_pos * s->cm);
            cairo_stroke(s->cairo_draw);

            // Write column headings
            double x = x0 + margin_h;

            // Date column
            if (col_widths[0] > 0) {
                cairo_move_to(s->cairo_draw, x * s->cm, y * s->cm);
                cairo_show_text(s->cairo_draw, "Date");
                x += col_widths[0];
            }

            // Magnitude column
            if (col_widths[1] > 0) {
                cairo_move_to(s->cairo_draw, x * s->cm, y * s->cm);
                cairo_show_text(s->cairo_draw, "Mag");
                x += col_widths[1];
            }

            // Phase column
            if (col_widths[2] > 0) {
                cairo_move_to(s->cairo_draw, x * s->cm, y * s->cm);
                cairo_show_text(s->cairo_draw, "Phase");
                x += col_widths[2];
            }

            // Angular size column
            if (col_widths[3] > 0) {
                cairo_move_to(s->cairo_draw, x * s->cm, y * s->cm);
                cairo_show_text(s->cairo_draw, "Angular size");
                x += col_widths[3];
            }
        }

        // Advance vertically below column headings
        y += line_height;
        legend_y_pos += line_height;

        // Keep track of which day we last saw
        int previous_day_of_month = -1;

        // Loop over ephemeris data points
        double latest_mag_displayed = e->data[0].mag;
        for (int j = 0; j < e->point_count; j++) {
            // Extract calendar date components for this ephemeris data point
            int year, month, day, hour, minute, status = 0;
            double second;
            inv_julian_day(e->data[j].jd,
                           &year, &month, &day, &hour, &minute, &second,
                           &status, temp_err_string);

            // Abort on error
            if (status) {
                stch_error(temp_err_string);
                continue;
            }

            // Reject this point if it is within same day as previous point
            if (day == previous_day_of_month) continue;
            previous_day_of_month = day;

            // Decide whether to include this point in our table
            // Force an update to the table if the brightness of the object has changed considerably
            int force_display = 0;
            if ((fabs(e->data[j].mag - latest_mag_displayed) > 1) &&
                ((day == 1) || (day == 7) || (day == 15) || (day != 21))) {
                force_display = 1;
                latest_mag_displayed = e->data[j].mag;
            }

            if (!force_display) {
                if (ephemeris_duration > 10 * 365) {
                    if ((day != 1) || (month != 1) || (year % 2)) continue;
                } else if (ephemeris_duration > 5 * 365) {
                    if ((day != 1) || (month != 1)) continue;
                } else if (ephemeris_duration > 2 * 365) {
                    if ((day != 1) || (((month - 1) % 6) != 0)) continue;
                } else if (ephemeris_duration > 365) {
                    if ((day != 1) || (((month - 1) % 3) != 0)) continue;
                } else if (ephemeris_duration > 170) {
                    if ((day != 1) || (((month - 1) % 2) != 0)) continue;
                } else if (ephemeris_duration > 100) {
                    if (day != 1) continue;
                } else if (ephemeris_duration > 60) {
                    if ((day != 1) && (day != 15)) continue;
                } else if (ephemeris_duration > 21) {
                    if ((day != 1) && (day != 7) && (day != 15) && (day != 21)) continue;
                } else if (ephemeris_duration > 14) {
                    if (((day - 1) % 4) != 0) continue;
                } else if (ephemeris_duration > 7) {
                    if (((day - 1) % 2) != 0) continue;
                }
            }

            // Make an entry in the table for this day
            double x = x0 + margin_h;
            if (draw_output) {
                cairo_select_font_face(s->cairo_draw, s->font_family, CAIRO_FONT_SLANT_NORMAL,
                                       CAIRO_FONT_WEIGHT_NORMAL);

                // Extract calendar date components for this ephemeris data point
                if (col_widths[0] > 0) {
                    sprintf(text_buffer, "%d %.3s %d", year, get_month_name(month), day);
                    cairo_move_to(s->cairo_draw, x * s->cm, y * s->cm);
                    cairo_show_text(s->cairo_draw, text_buffer);
                    x += col_widths[0];
                }

                // Magnitude column
                if (col_widths[1] > 0) {
                    sprintf(text_buffer, "%.1f", e->data[j].mag);
                    cairo_move_to(s->cairo_draw, x * s->cm, y * s->cm);
                    cairo_show_text(s->cairo_draw, text_buffer);
                    x += col_widths[1];
                }

                // Phase column
                if (col_widths[2] > 0) {
                    sprintf(text_buffer, "%.0f%%", e->data[j].phase * 100);
                    cairo_move_to(s->cairo_draw, x * s->cm, y * s->cm);
                    cairo_show_text(s->cairo_draw, text_buffer);
                    x += col_widths[2];
                }

                // Angular size column
                if (col_widths[3] > 0) {
                    sprintf(text_buffer, "%.1f%c", e->data[j].angular_size / (angular_size_unit == '"' ? 1 : 60),
                            angular_size_unit);
                    cairo_move_to(s->cairo_draw, x * s->cm, y * s->cm);
                    cairo_show_text(s->cairo_draw, text_buffer);
                    x += col_widths[3];
                }
            }

            // Move down to next row
            y += line_height;
            legend_y_pos += line_height;
        }

        if (draw_output) {
            double y1 = legend_y_pos - line_height;
            // Draw bottom table horizontal
            cairo_move_to(s->cairo_draw, x0 * s->cm, y1 * s->cm);
            cairo_line_to(s->cairo_draw, x2 * s->cm, y1 * s->cm);

            // Draw table verticals
            double x = x0;
            for (int k = 0; k <= 4; k++) {
                cairo_move_to(s->cairo_draw, x * s->cm, y0 * s->cm);
                cairo_line_to(s->cairo_draw, x * s->cm, y1 * s->cm);
                x += col_widths[k];
            }

            // Stroke all lines
            cairo_stroke(s->cairo_draw);
        }

        // Bottom margin
        legend_y_pos += 0.4;
    }

    return legend_y_pos;
}