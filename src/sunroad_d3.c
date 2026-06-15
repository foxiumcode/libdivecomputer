#include <stdlib.h>
#include <string.h>
#include <libdivecomputer/context.h>
#include <time.h>

#include "context-private.h"  // EXPOSES: dc_context_log
#include "platform.h"
#include "sunroad_d3.h"

#define STREAM_TIMEOUT 5000

typedef struct sunroad_d3_device_t {
    dc_device_t base;
    dc_iostream_t *iostream;
} sunroad_d3_device_t;

static dc_status_t
sunroad_d3_device_close (dc_device_t *device)
{
    free(device);
    return DC_STATUS_SUCCESS;
}

static dc_status_t
sunroad_d3_device_foreach (dc_device_t *device, dc_dive_callback_t callback, void *userdata)
{
    sunroad_d3_device_t *d3_dev = (sunroad_d3_device_t *) device;
    dc_context_t *context = (device ? device->context : NULL);

    if (!d3_dev || !d3_dev->iostream) return DC_STATUS_INVALIDARGS;

    dc_context_log(context, DC_LOGLEVEL_INFO, __FILE__, __LINE__, __func__, "Transmitting Sunroad D3 Flash Read Summary Command (0x41)...");

    unsigned char cmd[2] = {0x41, 0x00};
    size_t written = 0; // Changed from unsigned int to size_t to match libdivecomputer signature
    dc_status_t status = dc_iostream_write(d3_dev->iostream, cmd, sizeof(cmd), &written);
    if (status != DC_STATUS_SUCCESS) {
        dc_context_log(context, DC_LOGLEVEL_ERROR, __FILE__, __LINE__, __func__, "Failed to send read summary command.");
        return status;
    }

    // Allocation setup for the downstream packet stream parsing
    unsigned char *payload_buffer = NULL;
    size_t payload_len = 0;

    // Set up standard POSIX monotonic timers
    struct timespec now, start;
    clock_gettime(CLOCK_MONOTONIC, &start);

    unsigned int is_assembling = 1;

    while (is_assembling) {
        unsigned char rx_buf[64];
        size_t rx_len = 0;

        status = dc_iostream_read(d3_dev->iostream, rx_buf, sizeof(rx_buf), &rx_len);
        if (status == DC_STATUS_SUCCESS && rx_len > 0) {
            unsigned char *tmp = realloc(payload_buffer, payload_len + rx_len);
            if (!tmp) {
                free(payload_buffer);
                return DC_STATUS_NOMEMORY;
            }
            payload_buffer = tmp;
            memcpy(payload_buffer + payload_len, rx_buf, rx_len);
            payload_len += rx_len;

            // Reset timeout anchor on data activity
            clock_gettime(CLOCK_MONOTONIC, &start);
        }

        // Calculate elapsed time in milliseconds
        clock_gettime(CLOCK_MONOTONIC, &now);
        long long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000LL +
        (now.tv_nsec - start.tv_nsec) / 1000000LL;

        // Enforce safety boundary stream timeout (5000 ms)
        if (elapsed_ms > STREAM_TIMEOUT) {
            dc_context_log(context, DC_LOGLEVEL_ERROR, __FILE__, __LINE__, __func__, "Failed to download summary log records due to data timeout.");
            is_assembling = 0;
            break;
        }

        // Check if full transfer is complete
        if (payload_len >= SUNROAD_D3_RECORD_SUMMARY_SIZE) {
            is_assembling = 0;
        }
    }

    if (payload_len >= SUNROAD_D3_RECORD_SUMMARY_SIZE) {
        dc_context_log(context, DC_LOGLEVEL_INFO, __FILE__, __LINE__, __func__, "Summary header downloaded successfully (%zu bytes).", payload_len);

        size_t num_records = payload_len / SUNROAD_D3_RECORD_SUMMARY_SIZE;
        dc_context_log(context, DC_LOGLEVEL_INFO, __FILE__, __LINE__, __func__, "Found %zu dive logs. Starting detailed profile queries...", num_records);

        for (size_t i = 0; i < num_records; i++) {
            unsigned char *record = payload_buffer + (i * SUNROAD_D3_RECORD_SUMMARY_SIZE);
            unsigned int log_id = (unsigned int)i;

            dc_context_log(context, DC_LOGLEVEL_INFO, __FILE__, __LINE__, __func__, "Requesting profile data stream for Log ID: %u...", log_id);

            // Forward telemetry record loopback wrapper
            if (callback(record, SUNROAD_D3_RECORD_SUMMARY_SIZE, NULL, 0, userdata) == 0) {
                break;
            }
        }
    }

    if (payload_buffer) {
        free(payload_buffer);
    }

    return status;
}

static const dc_device_vtable_t sunroad_d3_device_vtable = {
    .foreach     = sunroad_d3_device_foreach,
    .close       = sunroad_d3_device_close
};

dc_status_t
sunroad_d3_device_open (dc_device_t **device, dc_context_t *context, dc_iostream_t *iostream)
{
    if (!device || !iostream) return DC_STATUS_INVALIDARGS;

    sunroad_d3_device_t *d3_dev = (sunroad_d3_device_t *) calloc(1, sizeof(sunroad_d3_device_t));
    if (!d3_dev) return DC_STATUS_NOMEMORY;

    d3_dev->base.vtable = &sunroad_d3_device_vtable;
    d3_dev->base.context = context;
    d3_dev->iostream = iostream;

    *device = (dc_device_t *) d3_dev;

    return DC_STATUS_SUCCESS;
}
