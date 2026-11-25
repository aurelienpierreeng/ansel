/*
    This file is part of Ansel,
    Copyright (C) 2026 Guillaume Stutin.

    Ansel is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Ansel is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Ansel.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef DT_PIXEL_DUST_H
#define DT_PIXEL_DUST_H

#include <stdint.h>

/**
 * @file dust.h
 * @brief Find a speck of dust in a painted zone, a place to heal it from, and its outline.
 *
 * A speck of dust blocks part of the light that reaches the sensor: in the linear values of a
 * scan or of a raw it is a dark spot, a bump of the density `-log10(value)` over its
 * surroundings. A film carries the opposite kind as well, a spot that let more light through than
 * its surroundings: dust that was on it when it was exposed, a pinhole, a scratch in the emulsion,
 * a dip of the density. Nothing in the pixels tells either from a detail of the image that looks
 * alike (sky between leaves, a pebble); the zone painted over it does. Within that zone this
 * module finds how far the speck reaches, which needs no knowledge of the image's meaning, only of
 * the density around the zone and of the grain.
 *
 * The thresholds are those measured on scans of a colour negative, recorded in
 * doc/iop-notes.md ("dust on a scan is a density peak, but only a click tells it from the
 * image"); their radii are full-resolution pixels, scaled by dt_dust_image_t.scale.
 */

/** What dt_dust_find_in_zone(), dt_dust_find_source() and dt_dust_outline() report. */
typedef enum dt_dust_status_t
{
  DT_DUST_OK = 0,
  DT_DUST_NOT_FOUND = 1, /**< nothing in the zone stands out of the grain enough to be a speck */
  DT_DUST_NO_SOURCE = 2, /**< every place around the speck holds a peak of its own */
  DT_DUST_NO_MEMORY = 3
} dt_dust_status_t;

/** The pixels a search reads: four interleaved channels, linear, the first three read. */
typedef struct dt_dust_image_t
{
  const float *rgba;
  int width;
  int height;
  /** buffer pixels per full-resolution pixel; the measured radii are multiplied by it */
  float scale;
} dt_dust_image_t;

/** A zone painted over a buffer, where a speck is looked for: box_width × box_height bytes at
 * (box_x, box_y), in the buffer's pixels, nonzero in the zone. What lies outside the buffer is
 * ignored. */
typedef struct dt_dust_zone_t
{
  const uint8_t *mask;
  int box_x;
  int box_y;
  int box_width;
  int box_height;
  /** the points the brush was painted through, stroke_count x, y pairs in the buffer's pixels, or
   * NULL: a scratch is followed along them, bending as they do, and along a straight line without */
  const float *stroke;
  int stroke_count;
} dt_dust_zone_t;

/** A speck found by dt_dust_find_in_zone(), in the pixel coordinates of the buffer it was found in. */
typedef struct dt_dust_speck_t
{
  /** box_width × box_height bytes, 1 on the speck; owned, released by dt_dust_speck_cleanup() */
  uint8_t *mask;
  int box_x;
  int box_y;
  int box_width;
  int box_height;
  float center[2]; /**< centroid of the mask */
  float radius;    /**< distance from the centroid to the farthest masked pixel, plus one pixel */
  /** distance from the mask's long axis to its farthest pixel, plus one pixel: the radius of a round
   * speck, half the width of a scratch */
  float half_width;
  /** 1 for a speck darker than the zone's background, a density above it; -1 for a lighter one */
  int polarity;
  float peak;      /**< the speck's largest density difference from the zone's background, in D */
  float contrast;  /**< that difference over the grain, in standard deviations */
} dt_dust_speck_t;

/**
 * @brief Find the speck of dust in a painted zone.
 *
 * The zone's background is the harmonic interpolation, channel by channel, of the densities
 * around it, taken as box means of three pixels so that their grain does not carry in; the excess
 * over it, averaged over the channels, is smoothed by boxes of up to eight pixels, so that a wide
 * speck that blocks no more light than the grain varies still stands out. The box and the polarity
 * where the excess, or its opposite for a light speck, peaks highest over the grain are kept, the
 * grain measured on the side of the excess that only it reaches, among the boxes the zone holds at
 * least sixteen of; both are read where the box fits in the zone. The speck is what stands above
 * three times the grain and a tenth of the peak there, connected to it, a pixel wider, within the
 * zone.
 *
 * A zone painted as a stroke, three times longer than wide or more, may be over a scratch: a line a
 * few pixels wide, often no more than twice the grain on one pixel, that only its length brings out.
 * It is looked for at angles within 22.5 degrees of the stroke, along segments of 33 pixels, as the
 * excess less its value 4 pixels away on the nearer side, where each third of the segment holds half
 * of the mean or more, so that a speck the segment crosses is no line; then measured along 65 pixels
 * at the angle found, against seven times the grain at least. From its peak it is followed each way
 * along the stroke's points, smoothed over 4 pixels, or along that angle without them: along the path
 * that stands out most, by steps of 8 pixels, each a pixel across at most from the last, darker or
 * lighter, as a scratch turns from one to the other along a film, its segments bending as the
 * stroke does. A step counts for how far its segment stands out beyond once the grain, up to four
 * times, less once per pixel moved across and three times where it turns darker or lighter. A way
 * ends where going on would leave the stroke, at the end that scores best: where the scratch leaves
 * the stroke, or at the stroke's end.
 * The scratch is 3 pixels each side of that path, a pixel wider. Of a speck and a scratch, the stroke
 * was painted over the one that stands out farther along it.
 *
 * A zone over grain alone may still find a speck in a slow variation of the image, as strong as a
 * faint speck is: the zone is the user's word that there is one to find.
 *
 * @param image the buffer to search
 * @param zone the zone painted, in the buffer's pixels
 * @param speck filled on DT_DUST_OK; left empty otherwise
 * @return DT_DUST_OK, DT_DUST_NOT_FOUND when the peak stands less than five times the grain out of
 *         it, or a scratch less than seven, or when the zone leaves no pixel around it, or
 *         DT_DUST_NO_MEMORY
 */
dt_dust_status_t dt_dust_find_in_zone(const dt_dust_image_t *image, const dt_dust_zone_t *zone,
                                      dt_dust_speck_t *speck);

/**
 * @brief Choose where to heal a speck from.
 *
 * Among the offsets of two to six half widths (dt_dust_speck_t.half_width: the radius of a round
 * speck), the one whose ring of eight pixels around the speck matches the ring around the speck
 * best, in density with each channel's mean difference set aside (a heal corrects the level, not
 * the texture), skipping offsets that leave part of the speck on itself, as a shift along a
 * scratch does, or that would bring another speck into it, dark or light: a top-hat, or a black
 * top-hat, six times the grain above its median, or, of the speck's own polarity, half as far as
 * the speck's own.
 *
 * @param image the buffer the speck was found in
 * @param speck as dt_dust_find_in_zone() returned it
 * @param offset the source minus the speck, in buffer pixels, on DT_DUST_OK
 * @return DT_DUST_OK, DT_DUST_NO_SOURCE or DT_DUST_NO_MEMORY
 */
dt_dust_status_t dt_dust_find_source(const dt_dust_image_t *image, const dt_dust_speck_t *speck, float offset[2]);

/**
 * @brief Trace the outline of a speck as a closed polygon.
 *
 * The outer boundary of the mask, followed along the pixel edges with diagonal neighbours joined,
 * then simplified to within a tolerance: the fewer vertices, the farther the polygon may cut into
 * the speck's rim or away from it. Holes are not traced: a polygon covers them.
 *
 * @param speck as dt_dust_find_in_zone() returned it
 * @param tolerance how far the polygon may stray from the mask's edge, in buffer pixels; never less
 *        than three quarters of one, the steps of the pixel edges, nor more than a third of the
 *        speck's half width, which would cut through a scratch
 * @param points on DT_DUST_OK, 2 × count floats (x, y in buffer pixels), freed with dt_free_align()
 * @param count number of vertices, at least 3
 * @return DT_DUST_OK or DT_DUST_NO_MEMORY
 */
dt_dust_status_t dt_dust_outline(const dt_dust_speck_t *speck, float tolerance, float **points, int *count);

/** Release what dt_dust_find_in_zone() allocated in a speck, and empty it. */
void dt_dust_speck_cleanup(dt_dust_speck_t *speck);

#endif // DT_PIXEL_DUST_H

// clang-format off
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.py
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-spaces modified;
// clang-format on
