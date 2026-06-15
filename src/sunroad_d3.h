#ifndef SUNROAD_D3_H
#define SUNROAD_D3_H

#include "device-private.h"

// Explicit data boundary layout definitions shared across translation units
#define SUNROAD_D3_RECORD_SUMMARY_SIZE   40   // Fixed 40-byte log summary header
#define SUNROAD_D3_SAMPLE_STRIDE_SIZE     4   // Time-series sample chunk size (Depth + Temp)

#ifdef __cplusplus
extern "C" {
    #endif

    dc_status_t
    sunroad_d3_device_open (dc_device_t **device, dc_context_t *context, dc_iostream_t *iostream);

    #ifdef __cplusplus
}
#endif

#endif /* SUNROAD_D3_H */
