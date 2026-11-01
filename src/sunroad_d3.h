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

#ifndef SUNROAD_D3_H
#define SUNROAD_D3_H

#include <libdivecomputer/context.h>
#include <libdivecomputer/device.h>
#include <libdivecomputer/parser.h>
#include <libdivecomputer/iostream.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define SUNROAD_D3_SUMMARY_SIZE       40
#define SUNROAD_D3_FINGERPRINT_OFFSET 1
#define SUNROAD_D3_FINGERPRINT_SIZE   4
#define SUNROAD_D3_MAX_LOGS           48

/*
 * Synthetic dive blob layout passed from sunroad_d3.c to sunroad_d3_parser.c:
 *   Bytes 0..39          : Raw 40-byte summary record (from 0x42 stream)
 *   Bytes 40..41         : uint16_t LE depth sample count (N_d)
 *   Bytes 42..43         : uint16_t LE temperature sample count (N_t)
 *   Bytes 44..44+N_d-1   : Raw 0x44 depth samples (1 byte per 5s tick, meters)
 *   Bytes 44+N_d..end    : Raw 0x46 temp samples (1 byte per 5s tick, Celsius)
 */
#define SUNROAD_D3_BLOB_HEADER_SIZE   44

dc_status_t
sunroad_d3_device_open (dc_device_t **out, dc_context_t *context, dc_iostream_t *iostream);

dc_status_t
sunroad_d3_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* SUNROAD_D3_H */
