// SPDX-License-Identifier: GPL-2.0
#include <string.h>
#include "dive.h"
#include "device.h"

/*
 * Good fake dive profiles are hard.
 *
 * "depthtime" is the integral of the dive depth over
 * time ("area" of the dive profile). We want that
 * area to match the average depth (avg_d*max_t).
 *
 * To do that, we generate a 6-point profile:
 *
 *  (0, 0)
 *  (t1, max_d)
 *  (t2, max_d)
 *  (t3, d)
 *  (t4, d)
 *  (max_t, 0)
 *
 * with the same ascent/descent rates between the
 * different depths.
 *
 * NOTE: avg_d, max_d and max_t are given constants.
 * The rest we can/should play around with to get a
 * good-looking profile.
 *
 * That six-point profile gives a total area of:
 *
 *   (max_d*max_t) - (max_d*t1) - (max_d-d)*(t4-t3)
 *
 * And the "same ascent/descent rates" requirement
 * gives us (time per depth must be same):
 *
 *   t1 / max_d = (t3-t2) / (max_d-d)
 *   t1 / max_d = (max_t-t4) / d
 *
 * We also obviously require:
 *
 *   0 <= t1 <= t2 <= t3 <= t4 <= max_t
 *
 * Let us call 'd_frac = d / max_d', and we get:
 *
 * Total area must match average depth-time:
 *
 *   (max_d*max_t) - (max_d*t1) - (max_d-d)*(t4-t3) = avg_d*max_t
 *      max_d*(max_t-t1-(1-d_frac)*(t4-t3)) = avg_d*max_t
 *             max_t-t1-(1-d_frac)*(t4-t3) = avg_d*max_t/max_d
 *                   t1+(1-d_frac)*(t4-t3) = max_t*(1-avg_d/max_d)
 *
 * and descent slope must match ascent slopes:
 *
 *   t1 / max_d = (t3-t2) / (max_d*(1-d_frac))
 *           t1 = (t3-t2)/(1-d_frac)
 *
 * and
 *
 *   t1 / max_d = (max_t-t4) / (max_d*d_frac)
 *           t1 = (max_t-t4)/d_frac
 *
 * In general, we have more free variables than we have constraints,
 * but we can aim for certain basics, like a good ascent slope.
 */
static int fill_samples(struct sample *s, int max_d, int avg_d, int max_t, double slope, double d_frac)
{
	double t_frac = max_t * (1 - avg_d / (double)max_d);
	int t1 = lrint(max_d / slope);
	int t4 = lrint(max_t - t1 * d_frac);
	int t3 = lrint(t4 - (t_frac - t1) / (1 - d_frac));
	int t2 = lrint(t3 - t1 * (1 - d_frac));

	if (t1 < 0 || t1 > t2 || t2 > t3 || t3 > t4 || t4 > max_t)
		return 0;

	s[1].time.seconds = t1;
	s[1].depth.mm = max_d;
	s[2].time.seconds = t2;
	s[2].depth.mm = max_d;
	s[3].time.seconds = t3;
	s[3].depth.mm = lrint(max_d * d_frac);
	s[4].time.seconds = t4;
	s[4].depth.mm = lrint(max_d * d_frac);

	return 1;
}

/* we have no average depth; instead of making up a random average depth
 * we should assume either a PADI rectangular profile (for short and/or
 * shallow dives) or more reasonably a six point profile with a 3 minute
 * safety stop at 5m */
static void fill_samples_no_avg(struct sample *s, int max_d, int max_t, double slope)
{
	// shallow or short dives are just trapecoids based on the given slope
	if (max_d < 10000 || max_t < 600) {
		s[1].time.seconds = lrint(max_d / slope);
		s[1].depth.mm = max_d;
		s[2].time.seconds = max_t - lrint(max_d / slope);
		s[2].depth.mm = max_d;
	} else {
		s[1].time.seconds = lrint(max_d / slope);
		s[1].depth.mm = max_d;
		s[2].time.seconds = max_t - lrint(max_d / slope) - 180;
		s[2].depth.mm = max_d;
		s[3].time.seconds = max_t - lrint(5000 / slope) - 180;
		s[3].depth.mm = 5000;
		s[4].time.seconds = max_t - lrint(5000 / slope);
		s[4].depth.mm = 5000;
	}
}

void fake_dc(struct divecomputer *dc)
{
	alloc_samples(dc, 6);
	struct sample *fake = dc->sample;
	int i;

	dc->samples = 6;

	/* The dive has no samples, so create a few fake ones */
	int max_t = dc->duration.seconds;
	int max_d = dc->maxdepth.mm;
	int avg_d = dc->meandepth.mm;

	memset(fake, 0, 6 * sizeof(struct sample));
	fake[5].time.seconds = max_t;
	for (i = 0; i < 6; i++) {
		fake[i].bearing.degrees = -1;
		fake[i].ndl.seconds = -1;
	}
	if (!max_t || !max_d) {
		dc->samples = 0;
		return;
	}

	/* Set last manually entered time to the total dive length */
	dc->last_manual_time = dc->duration;

	/*
	 * We want to fake the profile so that the average
	 * depth ends up correct. However, in the absence of
	 * a reasonable average, let's just make something
	 * up. Note that 'avg_d == max_d' is _not_ a reasonable
	 * average.
	 * We explicitly treat avg_d == 0 differently */
	if (avg_d == 0) {
		/* we try for a sane slope, but bow to the insanity of
		 * the user supplied data */
		fill_samples_no_avg(fake, max_d, max_t, MAX(2.0 * max_d / max_t, 5000.0 / 60));
		if (fake[3].time.seconds == 0) { // just a 4 point profile
			dc->samples = 4;
			fake[3].time.seconds = max_t;
		}
		return;
	}
	if (avg_d < max_d / 10 || avg_d >= max_d) {
		avg_d = (max_d + 10000) / 3;
		if (avg_d > max_d)
			avg_d = max_d * 2 / 3;
	}
	if (!avg_d)
		avg_d = 1;

	/*
	 * Ok, first we try a basic profile with a specific ascent
	 * rate (5 meters per minute) and d_frac (1/3).
	 */
	if (fill_samples(fake, max_d, avg_d, max_t, 5000.0 / 60, 0.33))
		return;

	/*
	 * Ok, assume that didn't work because we cannot make the
	 * average come out right because it was a quick deep dive
	 * followed by a much shallower region
	 */
	if (fill_samples(fake, max_d, avg_d, max_t, 10000.0 / 60, 0.10))
		return;

	/*
	 * Uhhuh. That didn't work. We'd need to find a good combination that
	 * satisfies our constraints. Currently, we don't, we just give insane
	 * slopes.
	 */
	if (fill_samples(fake, max_d, avg_d, max_t, 10000.0, 0.01))
		return;

	/* Even that didn't work? Give up, there's something wrong */
}

static void match_id(void *_dc, const char *model, uint32_t deviceid,
		     const char *nickname, const char *serial, const char *firmware)
{
	// here nickname is unused
	(void)nickname;

	struct divecomputer *dc = _dc;

	if (dc->deviceid != deviceid)
		return;
	if (!model || !dc->model || strcmp(dc->model, model))
		return;

	if (serial && !dc->serial)
		dc->serial = strdup(serial);
	if (firmware && !dc->fw_version)
		dc->fw_version = strdup(firmware);
}

/*
 * When setting the device ID, we also fill in the
 * serial number and firmware version data
 */
void set_dc_deviceid(struct divecomputer *dc, unsigned int deviceid)
{
	if (deviceid) {
		dc->deviceid = deviceid;
		call_for_each_dc(dc, match_id, false);
	}
}
