/*
 * libdivecomputer
 *
 * Copyright (C) 2026 Sonny Chen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 */

#include <stdlib.h>
#include <string.h>

#include "sunroad_d3.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"

#define ISINSTANCE(parser) dc_parser_isinstance((parser), &sunroad_d3_parser_vtable)

/*
 * Firmware FUN_000366e0_ble_opcode_main subtracts 0x7080 (28,800s = 8h)
 * from the watch's local wall-clock epoch when generating the record timestamp.
 */
#define SUNROAD_D3_FW_TZ_OFFSET 0x7080
#define SUNROAD_D3_DEPTH_SCALE  0.3

typedef struct sunroad_d3_parser_t {
	dc_parser_t base;
} sunroad_d3_parser_t;

static dc_status_t sunroad_d3_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t sunroad_d3_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t sunroad_d3_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static const dc_parser_vtable_t sunroad_d3_parser_vtable = {
	sizeof (sunroad_d3_parser_t),
	DC_FAMILY_SUNROAD_D3,
	NULL,                              /* set_clock */
	NULL,                              /* set_atmospheric */
	NULL,                              /* set_density */
	sunroad_d3_parser_get_datetime,    /* datetime */
	sunroad_d3_parser_get_field,       /* fields */
	sunroad_d3_parser_samples_foreach, /* samples_foreach */
	NULL                               /* destroy */
};

dc_status_t
sunroad_d3_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size)
{
	sunroad_d3_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	parser = (sunroad_d3_parser_t *) dc_parser_allocate (context, &sunroad_d3_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	*out = (dc_parser_t *) parser;
	return DC_STATUS_SUCCESS;
}

static dc_status_t
sunroad_d3_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	const unsigned char *data = abstract->data;

	if (abstract->size < SUNROAD_D3_SUMMARY_SIZE)
		return DC_STATUS_DATAFORMAT;

	/* Add back 0x7080 so dc_datetime_gmtime yields the watch's local wall-clock time */
	dc_ticks_t ticks = (dc_ticks_t) array_uint32_le (data + 1) + SUNROAD_D3_FW_TZ_OFFSET;

	if (!dc_datetime_gmtime (datetime, ticks))
		return DC_STATUS_DATAFORMAT;

	datetime->timezone = DC_TIMEZONE_NONE;
	return DC_STATUS_SUCCESS;
}

static dc_status_t
sunroad_d3_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	const unsigned char *data = abstract->data;

	if (abstract->size < SUNROAD_D3_SUMMARY_SIZE)
		return DC_STATUS_DATAFORMAT;

	if (value == NULL)
		return DC_STATUS_INVALIDARGS;

	unsigned int dive_mode = data[22];
	unsigned int depth_cnt = 0;
	unsigned int temp_cnt = 0;

	if (abstract->size >= SUNROAD_D3_BLOB_HEADER_SIZE) {
		depth_cnt = array_uint16_le (data + 40);
		temp_cnt = array_uint16_le (data + 42);
	}

	switch (type) {
	case DC_FIELD_DIVETIME:
		*((unsigned int *) value) = ((unsigned int) data[5] * 3600U) +
		                            ((unsigned int) data[6] * 60U) +
		                            (unsigned int) data[7];
		break;

	case DC_FIELD_MAXDEPTH:
		*((double *) value) = array_uint16_be (data + 14) * SUNROAD_D3_DEPTH_SCALE;
		break;

	case DC_FIELD_AVGDEPTH:
		*((double *) value) = array_uint16_be (data + 16) * SUNROAD_D3_DEPTH_SCALE;
		break;

	case DC_FIELD_DIVEMODE:
		switch (dive_mode) {
		case 0:
			*((dc_divemode_t *) value) = DC_DIVEMODE_FREEDIVE;
			break;
		case 1:
			*((dc_divemode_t *) value) = DC_DIVEMODE_OC;
			break;
		case 2:
			*((dc_divemode_t *) value) = DC_DIVEMODE_GAUGE;
			break;
		default:
			return DC_STATUS_DATAFORMAT;
		}
		break;

	case DC_FIELD_GASMIX_COUNT:
		*((unsigned int *) value) = (dive_mode == 1) ? 1 : 0;
		break;

	case DC_FIELD_GASMIX:
		if (dive_mode != 1 || flags >= 1)
			return DC_STATUS_UNSUPPORTED;
		{
			dc_gasmix_t *gasmix = (dc_gasmix_t *) value;
			unsigned int o2 = data[27];
			if (o2 < 21 || o2 > 100)
				o2 = 21;
			memset (gasmix, 0, sizeof (*gasmix));
			gasmix->helium = 0.0;
			gasmix->oxygen = (double) o2 / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
		}
		break;

	case DC_FIELD_TEMPERATURE_MINIMUM:
	case DC_FIELD_TEMPERATURE_MAXIMUM:
	case DC_FIELD_TEMPERATURE_SURFACE:
		if (temp_cnt == 0 || abstract->size < SUNROAD_D3_BLOB_HEADER_SIZE + depth_cnt + temp_cnt)
			return DC_STATUS_UNSUPPORTED;
		{
			const unsigned char *temps = data + SUNROAD_D3_BLOB_HEADER_SIZE + depth_cnt;
			unsigned int min_t = 0xFF;
			unsigned int max_t = 0;
			unsigned int first_t = 0xFF;

			for (unsigned int i = 0; i < temp_cnt; ++i) {
				unsigned int t = temps[i];
				if (t == 0xFF || t == 0x00)
					continue;
				if (first_t == 0xFF)
					first_t = t;
				if (t < min_t)
					min_t = t;
				if (t > max_t)
					max_t = t;
			}

			if (first_t == 0xFF)
				return DC_STATUS_UNSUPPORTED;

			if (type == DC_FIELD_TEMPERATURE_MINIMUM)
				*((double *) value) = (double) min_t;
			else if (type == DC_FIELD_TEMPERATURE_MAXIMUM)
				*((double *) value) = (double) max_t;
			else
				*((double *) value) = (double) first_t;
		}
		break;

	default:
		return DC_STATUS_UNSUPPORTED;
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
sunroad_d3_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	const unsigned char *data = abstract->data;

	if (abstract->size < SUNROAD_D3_BLOB_HEADER_SIZE)
		return DC_STATUS_SUCCESS;

	unsigned int depth_cnt = array_uint16_le (data + 40);
	unsigned int temp_cnt = array_uint16_le (data + 42);

	if (abstract->size < SUNROAD_D3_BLOB_HEADER_SIZE + depth_cnt + temp_cnt)
		return DC_STATUS_DATAFORMAT;

	const unsigned char *depths = data + SUNROAD_D3_BLOB_HEADER_SIZE;
	const unsigned char *temps = depths + depth_cnt;
	unsigned int nsamples = depth_cnt;
	double last_temp = -1.0;

	for (unsigned int i = 0; i < nsamples; ++i) {
		dc_sample_value_t sample = {0};

		/* Sample time in milliseconds (5-second ticks) */
		sample.time = (i + 1) * 5000U;
		if (callback)
			callback (DC_SAMPLE_TIME, &sample, userdata);

		/* Depth sample (1 byte per 5s tick, in meters) */
		sample.depth = (depths[i] == 0xFF) ? 0.0 : (double) depths[i];
		if (callback)
			callback (DC_SAMPLE_DEPTH, &sample, userdata);

		/* Temperature sample (1 byte per 5s tick, in Celsius; filter 0xFF erased flash) */
		if (i < temp_cnt && temps[i] != 0xFF && temps[i] != 0x00) {
			last_temp = (double) temps[i];
		}
		if (last_temp >= 0.0) {
			sample.temperature = last_temp;
			if (callback)
				callback (DC_SAMPLE_TEMPERATURE, &sample, userdata);
		}
	}

	return DC_STATUS_SUCCESS;
}
