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
#include "device-private.h"
#include "array.h"
#include "platform.h"

#define ISINSTANCE(device) dc_device_isinstance((device), &sunroad_d3_device_vtable)

#define SUNROAD_CMD_SIZE          16
#define SUNROAD_PREAMBLE          0xBC
#define SUNROAD_OP_SUMMARY_REQ    0x41
#define SUNROAD_OP_SUMMARY_RSP    0x42
#define SUNROAD_OP_DEPTH_REQ      0x43
#define SUNROAD_OP_DEPTH_RSP      0x44
#define SUNROAD_OP_TEMP_REQ       0x45
#define SUNROAD_OP_TEMP_RSP       0x46

#define SUNROAD_REC_START_MARKER  0x28
#define SUNROAD_REC_END_MARKER    0xA5

#define SUNROAD_SUMMARY_TIMEOUT   3500
#define SUNROAD_PROFILE_TIMEOUT   2500

typedef struct sunroad_d3_device_t {
	dc_device_t base;
	dc_iostream_t *iostream;
	unsigned char fingerprint[SUNROAD_D3_FINGERPRINT_SIZE];
} sunroad_d3_device_t;

static dc_status_t sunroad_d3_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size);
static dc_status_t sunroad_d3_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata);
static dc_status_t sunroad_d3_device_close (dc_device_t *abstract);

static const dc_device_vtable_t sunroad_d3_device_vtable = {
	sizeof (sunroad_d3_device_t),
	DC_FAMILY_SUNROAD_D3,
	sunroad_d3_device_set_fingerprint, /* set_fingerprint */
	NULL,                              /* read */
	NULL,                              /* write */
	NULL,                              /* dump */
	sunroad_d3_device_foreach,         /* foreach */
	NULL,                              /* timesync */
	sunroad_d3_device_close,           /* close */
};

static void
sunroad_d3_build_cmd (unsigned char cmd[SUNROAD_CMD_SIZE], unsigned char opcode, unsigned int timestamp)
{
	unsigned int sum = 0;

	memset (cmd, 0, SUNROAD_CMD_SIZE);
	cmd[0] = SUNROAD_PREAMBLE;
	cmd[1] = opcode;
	cmd[2] = 0x01;
	cmd[3] = 0x00;
	cmd[6] = (unsigned char) (timestamp & 0xFF);
	cmd[7] = (unsigned char) ((timestamp >> 8) & 0xFF);
	cmd[8] = (unsigned char) ((timestamp >> 16) & 0xFF);
	cmd[9] = (unsigned char) ((timestamp >> 24) & 0xFF);

	for (unsigned int i = 0; i < SUNROAD_CMD_SIZE - 1; ++i)
		sum += cmd[i];

	cmd[SUNROAD_CMD_SIZE - 1] = (unsigned char) (sum & 0xFF);
}

static int
sunroad_d3_is_empty_rsp (const unsigned char *pkt, size_t len, unsigned char expected_op)
{
	return (len == 7 &&
	        pkt[0] == SUNROAD_PREAMBLE &&
	        pkt[1] == expected_op &&
	        pkt[2] == 0x01 &&
	        pkt[3] == 0x00 &&
	        pkt[4] == 0x00 &&
	        pkt[5] == 0x00 &&
	        pkt[6] == 0x00);
}

static int
sunroad_d3_is_valid_summary (const unsigned char rec[SUNROAD_D3_SUMMARY_SIZE])
{
	if (rec[0] != SUNROAD_REC_START_MARKER || rec[39] != SUNROAD_REC_END_MARKER)
		return 0;

	/* Bytes 28..38 are hardcoded 0x00 padding in FUN_000366e0_ble_opcode_main */
	for (unsigned int i = 28; i <= 38; ++i) {
		if (rec[i] != 0x00)
			return 0;
	}

	/* Validate HH:MM:SS bounds for Dive Duration, Surface Interval, and Dive End Time */
	if (rec[5] >= 24 || rec[6] >= 60 || rec[7] >= 60)
		return 0;
	if (rec[8] >= 100 || rec[9] >= 60 || rec[10] >= 60)
		return 0;
	if (rec[11] >= 24 || rec[12] >= 60 || rec[13] >= 60)
		return 0;

	return 1;
}

static dc_status_t
sunroad_d3_fetch_summaries (sunroad_d3_device_t *device, unsigned int since_ts,
                            unsigned char **out_buf, size_t *out_len)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	unsigned char cmd[SUNROAD_CMD_SIZE];
	unsigned char pkt[64];
	size_t nbytes = 0;
	unsigned char *buf = NULL;
	size_t expected_len = 0;
	size_t collected = 0;

	*out_buf = NULL;
	*out_len = 0;

	sunroad_d3_build_cmd (cmd, SUNROAD_OP_SUMMARY_REQ, since_ts);
	status = dc_iostream_write (device->iostream, cmd, sizeof (cmd), NULL);
	if (status != DC_STATUS_SUCCESS) {
		ERROR (device->base.context, "Failed to send 0x41 summary request.");
		return status;
	}

	dc_iostream_set_timeout (device->iostream, SUNROAD_SUMMARY_TIMEOUT);

	while (!device_is_cancelled (&device->base)) {
		nbytes = 0;
		status = dc_iostream_read (device->iostream, pkt, sizeof (pkt), &nbytes);
		if (status == DC_STATUS_TIMEOUT) {
			if (collected > 0) {
				WARNING (device->base.context,
				         "Summary stream timed out at %zu/%zu bytes; recovering intact records.",
				         collected, expected_len);
				status = DC_STATUS_SUCCESS;
			}
			break;
		}
		if (status != DC_STATUS_SUCCESS) {
			free (buf);
			return status;
		}
		if (nbytes == 0)
			continue;

		if (expected_len == 0) {
			if (sunroad_d3_is_empty_rsp (pkt, nbytes, SUNROAD_OP_SUMMARY_RSP)) {
				return DC_STATUS_SUCCESS;
			}
			if (nbytes >= 7 && pkt[0] == SUNROAD_PREAMBLE && pkt[1] == SUNROAD_OP_SUMMARY_RSP) {
				expected_len = array_uint16_le (pkt + 2);
				if (expected_len == 0 || expected_len > SUNROAD_D3_MAX_LOGS * SUNROAD_D3_SUMMARY_SIZE) {
					expected_len = SUNROAD_D3_MAX_LOGS * SUNROAD_D3_SUMMARY_SIZE;
				}
				buf = (unsigned char *) malloc (expected_len + 64);
				if (buf == NULL)
					return DC_STATUS_NOMEMORY;

				if (nbytes > 7) {
					size_t copy_len = nbytes - 7;
					memcpy (buf, pkt + 7, copy_len);
					collected += copy_len;
				}
			}
		} else {
			if (collected + nbytes <= expected_len + 64) {
				memcpy (buf + collected, pkt, nbytes);
				collected += nbytes;
			}
		}

		if (expected_len > 0 && collected >= expected_len)
			break;
	}

	*out_buf = buf;
	*out_len = collected;
	return status;
}

static dc_status_t
sunroad_d3_fetch_profile (sunroad_d3_device_t *device, unsigned char req_op, unsigned char rsp_op,
                          unsigned int timestamp, unsigned int max_samples,
                          unsigned char **out_samples, unsigned short *out_count)
{
	dc_status_t status = DC_STATUS_SUCCESS;
	unsigned char cmd[SUNROAD_CMD_SIZE];
	unsigned char pkt[64];
	size_t nbytes = 0;
	unsigned char *samples = NULL;
	unsigned int declared_samples = 0;
	unsigned int collected = 0;
	int stream_started = 0;

	*out_samples = NULL;
	*out_count = 0;

	sunroad_d3_build_cmd (cmd, req_op, timestamp);
	status = dc_iostream_write (device->iostream, cmd, sizeof (cmd), NULL);
	if (status != DC_STATUS_SUCCESS)
		return status;

	dc_iostream_set_timeout (device->iostream, SUNROAD_PROFILE_TIMEOUT);

	while (!device_is_cancelled (&device->base)) {
		nbytes = 0;
		status = dc_iostream_read (device->iostream, pkt, sizeof (pkt), &nbytes);
		if (status == DC_STATUS_TIMEOUT) {
			status = DC_STATUS_SUCCESS;
			break;
		}
		if (status != DC_STATUS_SUCCESS) {
			free (samples);
			return status;
		}
		if (nbytes == 0)
			continue;

		if (sunroad_d3_is_empty_rsp (pkt, nbytes, rsp_op)) {
			free (samples);
			return DC_STATUS_SUCCESS;
		}

		/* Stage 1: 11-byte Command ACK ending in 00 05 01 */
		if (nbytes == 11 && pkt[0] == SUNROAD_PREAMBLE && pkt[1] == rsp_op &&
		    pkt[8] == 0x00 && pkt[9] == 0x05 && pkt[10] == 0x01) {
			declared_samples = array_uint16_le (pkt + 2);
			continue;
		}

		/* Stage 2: First data notification starts with 8-byte header bc [44|46] cnt_lo cnt_hi csum_lo csum_hi 00 00 */
		if (!stream_started && nbytes >= 8 && pkt[0] == SUNROAD_PREAMBLE &&
		    pkt[1] == rsp_op && pkt[6] == 0x00 && pkt[7] == 0x00) {
			declared_samples = array_uint16_le (pkt + 2);
			if (declared_samples == 0)
				break;
			if (declared_samples > 16384)
				declared_samples = 16384;

			samples = (unsigned char *) malloc (declared_samples + 64);
			if (samples == NULL)
				return DC_STATUS_NOMEMORY;

			stream_started = 1;
			if (nbytes > 8) {
				size_t copy_len = nbytes - 8;
				memcpy (samples, pkt + 8, copy_len);
				collected += (unsigned int) copy_len;
			}
		} else if (stream_started && samples != NULL) {
			if (collected + nbytes <= declared_samples + 64) {
				memcpy (samples + collected, pkt, nbytes);
				collected += (unsigned int) nbytes;
			}
		}

		if (stream_started && collected >= declared_samples)
			break;
	}

	if (collected > declared_samples && declared_samples > 0)
		collected = declared_samples;
	if (max_samples > 0 && collected > max_samples)
		collected = max_samples;

	*out_samples = samples;
	*out_count = (unsigned short) collected;
	return status;
}

dc_status_t
sunroad_d3_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream)
{
	sunroad_d3_device_t *device = NULL;

	if (out == NULL || iostream == NULL)
		return DC_STATUS_INVALIDARGS;

	device = (sunroad_d3_device_t *) dc_device_allocate (context, &sunroad_d3_device_vtable);
	if (device == NULL)
		return DC_STATUS_NOMEMORY;

	device->iostream = iostream;
	memset (device->fingerprint, 0, sizeof (device->fingerprint));

	*out = (dc_device_t *) device;
	return DC_STATUS_SUCCESS;
}

static dc_status_t
sunroad_d3_device_set_fingerprint (dc_device_t *abstract, const unsigned char data[], unsigned int size)
{
	sunroad_d3_device_t *device = (sunroad_d3_device_t *) abstract;

	if (size && size != sizeof (device->fingerprint))
		return DC_STATUS_INVALIDARGS;

	if (size)
		memcpy (device->fingerprint, data, sizeof (device->fingerprint));
	else
		memset (device->fingerprint, 0, sizeof (device->fingerprint));

	return DC_STATUS_SUCCESS;
}

static dc_status_t
sunroad_d3_device_foreach (dc_device_t *abstract, dc_dive_callback_t callback, void *userdata)
{
	sunroad_d3_device_t *device = (sunroad_d3_device_t *) abstract;
	dc_status_t status = DC_STATUS_SUCCESS;
	unsigned char *raw_dir = NULL;
	size_t raw_dir_len = 0;
	unsigned char records[SUNROAD_D3_MAX_LOGS][SUNROAD_D3_SUMMARY_SIZE];
	unsigned int num_records = 0;
	unsigned int fp_ts = array_uint32_le (device->fingerprint);

	dc_platform_sleep (300);

	status = sunroad_d3_fetch_summaries (device, fp_ts, &raw_dir, &raw_dir_len);
	if (status != DC_STATUS_SUCCESS)
		return status;

	/* Scan and validate 40-byte records with 1-byte self-resynchronization */
	size_t idx = 0;
	while (raw_dir != NULL && idx + SUNROAD_D3_SUMMARY_SIZE <= raw_dir_len &&
	       num_records < SUNROAD_D3_MAX_LOGS) {
		if (sunroad_d3_is_valid_summary (raw_dir + idx)) {
			unsigned int ts = array_uint32_le (raw_dir + idx + 1);
			if (fp_ts != 0 && ts <= fp_ts) {
				idx += SUNROAD_D3_SUMMARY_SIZE;
				continue;
			}
			memcpy (records[num_records], raw_dir + idx, SUNROAD_D3_SUMMARY_SIZE);
			num_records++;
			idx += SUNROAD_D3_SUMMARY_SIZE;
		} else {
			idx++;
		}
	}
	free (raw_dir);

	dc_event_progress_t progress = EVENT_PROGRESS_INITIALIZER;
	progress.maximum = num_records > 0 ? num_records : 1;
	progress.current = 0;
	device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

	for (unsigned int i = 0; i < num_records; ++i) {
		if (device_is_cancelled (abstract))
			return DC_STATUS_CANCELLED;

		const unsigned char *rec = records[i];
		unsigned int ts = array_uint32_le (rec + 1);
		unsigned int dur_sec = ((unsigned int) rec[5] * 3600U) +
		                       ((unsigned int) rec[6] * 60U) +
		                       (unsigned int) rec[7];
		unsigned int expected_samples = dur_sec / 5U;

		unsigned char *depth_samples = NULL;
		unsigned short depth_cnt = 0;
		unsigned char *temp_samples = NULL;
		unsigned short temp_cnt = 0;

		sunroad_d3_fetch_profile (device, SUNROAD_OP_DEPTH_REQ, SUNROAD_OP_DEPTH_RSP,
		                          ts, expected_samples, &depth_samples, &depth_cnt);
		sunroad_d3_fetch_profile (device, SUNROAD_OP_TEMP_REQ, SUNROAD_OP_TEMP_RSP,
		                          ts, expected_samples, &temp_samples, &temp_cnt);

		size_t blob_size = SUNROAD_D3_BLOB_HEADER_SIZE + depth_cnt + temp_cnt;
		unsigned char *blob = (unsigned char *) malloc (blob_size);
		if (blob == NULL) {
			free (depth_samples);
			free (temp_samples);
			return DC_STATUS_NOMEMORY;
		}

		memcpy (blob, rec, SUNROAD_D3_SUMMARY_SIZE);
		blob[40] = (unsigned char) (depth_cnt & 0xFF);
		blob[41] = (unsigned char) ((depth_cnt >> 8) & 0xFF);
		blob[42] = (unsigned char) (temp_cnt & 0xFF);
		blob[43] = (unsigned char) ((temp_cnt >> 8) & 0xFF);
		if (depth_cnt > 0 && depth_samples != NULL)
			memcpy (blob + SUNROAD_D3_BLOB_HEADER_SIZE, depth_samples, depth_cnt);
		if (temp_cnt > 0 && temp_samples != NULL)
			memcpy (blob + SUNROAD_D3_BLOB_HEADER_SIZE + depth_cnt, temp_samples, temp_cnt);

		free (depth_samples);
		free (temp_samples);

		progress.current = i + 1;
		device_event_emit (abstract, DC_EVENT_PROGRESS, &progress);

		if (callback && !callback (blob, (unsigned int) blob_size,
		                           blob + SUNROAD_D3_FINGERPRINT_OFFSET,
		                           SUNROAD_D3_FINGERPRINT_SIZE, userdata)) {
			free (blob);
			break;
		}
		free (blob);
	}

	return DC_STATUS_SUCCESS;
}

static dc_status_t
sunroad_d3_device_close (dc_device_t *abstract)
{
	return DC_STATUS_SUCCESS;
}
