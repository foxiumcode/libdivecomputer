#include <stdlib.h>
#include <string.h>

#include "sunroad_d3.h"
#include "context-private.h"
#include "parser-private.h"
#include "platform.h"

typedef struct sunroad_parser_t sunroad_parser_t;

struct sunroad_parser_t {
    dc_parser_t base;
};

static unsigned short
array_uint16_le (const unsigned char *p)
{
    return p[0] | (p[1] << 8);
}

static unsigned int
array_uint32_le (const unsigned char *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
}

static void
epoch_to_datetime (unsigned int timestamp, dc_datetime_t *datetime)
{
    unsigned int seconds = timestamp;
    datetime->second = seconds % 60;
    unsigned int minutes = seconds / 60;
    datetime->minute = minutes % 60;
    unsigned int hours = minutes / 60;
    datetime->hour = hours % 24;
    unsigned int days = hours / 24;

    int y = 1970;
    while (1) {
        int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 1 : 0;
        unsigned int days_in_year = leap ? 366 : 365;
        if (days < days_in_year) {
            break;
        }
        days -= days_in_year;
        y++;
    }
    datetime->year = y;

    int leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)) ? 1 : 0;
    int months[12] = {31, leap ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

    int m = 0;
    while (days >= (unsigned int)months[m]) {
        days -= months[m];
        m++;
    }
    datetime->month = m + 1;
    datetime->day = days + 1;
}

static dc_status_t
sunroad_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
    sunroad_parser_t *parser = (sunroad_parser_t *) abstract;
    const unsigned char *data = abstract->data;
    size_t size = abstract->size;

    if (!parser || !data || size < SUNROAD_D3_RECORD_SUMMARY_SIZE) {
        return DC_STATUS_INVALIDARGS;
    }

    unsigned int timestamp = array_uint32_le(data + 0);
    epoch_to_datetime(timestamp, datetime);

    datetime->timezone = DC_TIMEZONE_NONE;

    return DC_STATUS_SUCCESS;
}

static dc_status_t
sunroad_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
    sunroad_parser_t *parser = (sunroad_parser_t *) abstract;
    const unsigned char *data = abstract->data;
    size_t size = abstract->size;

    if (!parser || !data || size < SUNROAD_D3_RECORD_SUMMARY_SIZE) {
        return DC_STATUS_INVALIDARGS;
    }

    if (value) {
        switch (type) {
            case DC_FIELD_DIVETIME:
            {
                unsigned int duration_sec = array_uint16_le(data + 4);
                *(unsigned int *) value = duration_sec;
                break;
            }
            case DC_FIELD_MAXDEPTH:
            {
                unsigned short max_depth_raw = array_uint16_le(data + 6);
                *(double *) value = max_depth_raw * 0.1;
                break;
            }
            case DC_FIELD_DIVEMODE:
            {
                unsigned int type_code = data[13] >> 4;
                if (type_code == 1) {
                    *(dc_divemode_t *) value = DC_DIVEMODE_OC;
                } else if (type_code == 2) {
                    *(dc_divemode_t *) value = DC_DIVEMODE_FREEDIVE;
                } else if (type_code == 3) {
                    *(dc_divemode_t *) value = DC_DIVEMODE_GAUGE;
                } else {
                    *(dc_divemode_t *) value = DC_DIVEMODE_OC;
                }
                break;
            }
            default:
                return DC_STATUS_UNSUPPORTED;
        }
    }

    return DC_STATUS_SUCCESS;
}

static dc_status_t
sunroad_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
    sunroad_parser_t *parser = (sunroad_parser_t *) abstract;
    const unsigned char *data = abstract->data;
    size_t size = abstract->size;

    if (!parser || !data) return DC_STATUS_INVALIDARGS;

    if (size <= SUNROAD_D3_RECORD_SUMMARY_SIZE) {
        return DC_STATUS_SUCCESS;
    }

    const unsigned char *samples = data + SUNROAD_D3_RECORD_SUMMARY_SIZE;
    unsigned int samples_size = size - SUNROAD_D3_RECORD_SUMMARY_SIZE;
    unsigned int offset = 0;

    while (offset + SUNROAD_D3_SAMPLE_STRIDE_SIZE <= samples_size) {
        dc_sample_value_t value = {0};

        value.time = (offset / SUNROAD_D3_SAMPLE_STRIDE_SIZE) * 4 * 1000;
        if (callback) callback (DC_SAMPLE_TIME, &value, userdata);

        unsigned short raw_depth = array_uint16_le(samples + offset);
        value.depth = raw_depth * 0.3;
        if (callback) callback (DC_SAMPLE_DEPTH, &value, userdata);

        unsigned short raw_temp = array_uint16_le(samples + offset + 2);
        value.temperature = (double)raw_temp / 100.0;
        if (callback) callback (DC_SAMPLE_TEMPERATURE, &value, userdata);

        offset += SUNROAD_D3_SAMPLE_STRIDE_SIZE;
    }

    return DC_STATUS_SUCCESS;
}

static const dc_parser_vtable_t sunroad_parser_vtable = {
    sizeof(sunroad_parser_t),
    DC_FAMILY_SUNROAD,
    NULL, /* set_clock */
    NULL, /* set_atmospheric */
    NULL, /* set_density */
    sunroad_parser_get_datetime, /* datetime */
    sunroad_parser_get_field, /* fields */
    sunroad_parser_samples_foreach, /* samples_foreach */
    NULL /* destroy */
};

dc_status_t
sunroad_parser_create (dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size)
{
    sunroad_parser_t *parser = NULL;

    if (out == NULL)
        return DC_STATUS_INVALIDARGS;

    parser = (sunroad_parser_t *) dc_parser_allocate (context, &sunroad_parser_vtable, data, size);
    if (parser == NULL) {
        ERROR (context, "Failed to allocate memory.");
        return DC_STATUS_NOMEMORY;
    }

    *out = (dc_parser_t *) parser;

    return DC_STATUS_SUCCESS;
}
