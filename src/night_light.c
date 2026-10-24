#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include "color.h"
#include "config.h"
#include "core.h"
#include "output.h"

// Of white in sRGB and BT.2020, which shows the colors as they are.
#define NEUTRAL_TEMPERATURE 6500
// How often the temperature changes during the transitions
#define TRANSITION_STEP_SECONDS 10

// The color of a black body at the temperature in K, from 1667 to 25000, in
// the linear colors of the primaries with a luminance of 1. The chromaticity
// is from the cubic spline approximation of the Planckian locus by Kim et al.
// in https://en.wikipedia.org/wiki/Planckian_locus#Approximation
static void
blackbody_color(double t, const struct color_primaries *primaries, double rgb[3])
{
	double x = t <= 4000
		? -0.2661239e9 / (t * t * t) - 0.2343589e6 / (t * t)
			+ 0.8776956e3 / t + 0.179910
		: -3.0258469e9 / (t * t * t) + 2.1070379e6 / (t * t)
			+ 0.2226347e3 / t + 0.240390;
	double y = t <= 2222
		? -1.1064814 * x * x * x - 1.34811020 * x * x
			+ 2.18555832 * x - 0.20219683
		: t <= 4000
		? -0.9549476 * x * x * x - 1.37418593 * x * x
			+ 2.09137015 * x - 0.16748867
		: 3.0817580 * x * x * x - 5.87338670 * x * x
			+ 3.75112997 * x - 0.37001483;
	double xyz[3] = {x / y, 1, (1 - x - y) / y};
	color_xyz_to_rgb(primaries, xyz, rgb);
	// Low temperatures are out of the gamut, without blue.
	for (int i = 0; i < 3; i++) {
		rgb[i] = fmax(rgb[i], 0);
	}
}

// The encoded value of the channel scaled by the factor, in the colors of the
// image description.
static uint16_t
scale_channel(const struct image_description *description, double value,
	double factor)
{
	double linear = color_eotf(description, value) * factor;
	return (uint16_t)lround(color_inverse_eotf(description, linear) * 0xffff);
}

// Scales the channels of the output to show white as a black body of the
// temperature, which is as bright as it can be. The black body of the neutral
// temperature is shown as white. The channels are scaled linearly, and
// encoded like the images of the output.
static uint32_t
create_gamma_lut(const struct output *output, int temperature)
{
	const struct image_description *description = output->image_description;
	double color[3];
	double neutral[3];
	blackbody_color(temperature, &description->primaries, color);
	blackbody_color(NEUTRAL_TEMPERATURE, &description->primaries, neutral);
	double factors[3];
	double max = 0;
	for (int i = 0; i < 3; i++) {
		factors[i] = color[i] / neutral[i];
		max = fmax(max, factors[i]);
	}
	uint32_t size = output->gamma_lut_size;
	struct drm_color_lut *lut = calloc(size, sizeof(*lut));
	assert(lut);
	for (uint32_t i = 0; i < size; i++) {
		double value = (double)i / (size - 1);
		lut[i] = (struct drm_color_lut){
			.red = scale_channel(description, value, factors[0] / max),
			.green = scale_channel(description, value, factors[1] / max),
			.blue = scale_channel(description, value, factors[2] / max),
		};
	}
	uint32_t blob = 0;
	assert(drmModeCreatePropertyBlob(core.kms.fd, lut, size * sizeof(*lut),
		&blob) == 0);
	free(lut);
	return blob;
}

// Sets the gamma LUT of the output for the temperature and its colors, which
// the next commit shows. Outputs whose CRTCs have none show the colors as
// they are, and those that have never been enabled get theirs with their
// colors.
void
night_light_update_output(struct output *output)
{
	if (!output->props.crtc_gamma_lut || !output->image_description) {
		return;
	}
	// The CRTC holds its own reference to the old blob until the commit
	// replaces it.
	if (output->gamma_lut_blob) {
		drmModeDestroyPropertyBlob(core.kms.fd, output->gamma_lut_blob);
	}
	output->gamma_lut_blob =
		core.night_light.temperature == NEUTRAL_TEMPERATURE ? 0
		: create_gamma_lut(output, core.night_light.temperature);
	output->needs_commit = true;
}

// How far the night has come at the time, from 0 by day to 1 at night. It
// rises over the transition from the start of the night, and falls over the
// transition from its end. A night that ends when it starts lasts all day.
static double
night_progress(time_t time)
{
	if (config.night_light.start == config.night_light.end) {
		return 1;
	}
	struct tm tm;
	assert(localtime_r(&time, &tm));
	int day = 24 * 60 * 60;
	int second = tm.tm_hour * 60 * 60 + tm.tm_min * 60 + tm.tm_sec;
	int start = config.night_light.start * 60;
	int night = (config.night_light.end * 60 - start + day) % day;
	double transition = config.night_light.transition * 60;
	int since_start = (second - start + day) % day;
	if (since_start < night) {
		return fmin(since_start / transition, 1);
	}
	return fmax(1 - (since_start - night) / transition, 0);
}

// The next time after now that the local clock shows the minute of the day.
static time_t
next_local_time(time_t now, int minute)
{
	struct tm today;
	assert(localtime_r(&now, &today));
	for (int day = today.tm_mday;; day++) {
		struct tm tm = {
			.tm_year = today.tm_year,
			.tm_mon = today.tm_mon,
			.tm_mday = day,
			.tm_hour = minute / 60,
			.tm_min = minute % 60,
			.tm_isdst = -1,
		};
		time_t time = mktime(&tm);
		if (time > now) {
			return time;
		}
	}
}

// Sets the temperature for now, and arms the timer for the next change.
void
night_light_update(void)
{
	time_t now = time(NULL);
	double progress = night_progress(now);
	int temperature = NEUTRAL_TEMPERATURE - (int)lround(progress
		* (NEUTRAL_TEMPERATURE - config.night_light.temperature));
	if (temperature != core.night_light.temperature) {
		core.night_light.temperature = temperature;
		struct output *output;
		wl_list_for_each(output, &core.outputs, link) {
			night_light_update_output(output);
		}
	}
	time_t wakeup = now + TRANSITION_STEP_SECONDS;
	if (night_progress(wakeup) == progress) {
		wakeup = MIN(next_local_time(now, config.night_light.start),
			next_local_time(now, config.night_light.end));
	}
	struct itimerspec spec = {.it_value = {.tv_sec = wakeup}};
	assert(timerfd_settime(core.night_light.timer_fd,
		TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &spec, NULL) == 0);
}

// Also wakes up when the clock is set, like by NTP.
static int
handle_timer(int fd, uint32_t mask, void *data)
{
	uint64_t expirations;
	assert(read(fd, &expirations, sizeof(expirations))
		== sizeof(expirations) || errno == ECANCELED);
	night_light_update();
	return 0;
}

// Sets the gamma LUTs of the outputs that have colors, and the others get
// theirs once they are configured.
void
night_light_init(void)
{
	core.night_light.temperature = NEUTRAL_TEMPERATURE;
	core.night_light.timer_fd = timerfd_create(CLOCK_REALTIME,
		TFD_CLOEXEC | TFD_NONBLOCK);
	assert(core.night_light.timer_fd >= 0);
	core.night_light.timer_source = wl_event_loop_add_fd(core.wl_loop,
		core.night_light.timer_fd, WL_EVENT_READABLE, handle_timer, NULL);
	assert(core.night_light.timer_source);
	if (config.night_light.temperature != NEUTRAL_TEMPERATURE) {
		night_light_update();
	}
}

void
night_light_fini(void)
{
	wl_event_source_remove(core.night_light.timer_source);
	close(core.night_light.timer_fd);
}
