#!/usr/bin/env python3
import asyncio
import ctypes
import queue
import threading
from bleak import BleakClient

DEVICE_MAC = "F3:75:6E:7C:25:BA"
HCI_ADAPTER = "hci0"
VENDOR_WRITE_UUID  = "de5bf72a-d711-4e47-af26-65e3012a5dc7"
VENDOR_NOTIFY_UUID = "de5bf729-d711-4e47-af26-65e3012a5dc7"
LIBDC_PATH = "/tmp/libdivecomputer/src/.libs/libdivecomputer.so"

libdc = ctypes.CDLL(LIBDC_PATH)

DC_STATUS_SUCCESS = 0
DC_STATUS_TIMEOUT = -7
DC_TRANSPORT_BLE = (1 << 5)
DC_FIELD_DIVETIME = 0
DC_FIELD_MAXDEPTH = 1
DC_FIELD_AVGDEPTH = 2
DC_FIELD_TEMPERATURE_MINIMUM = 5


class dc_datetime_t(ctypes.Structure):
    _fields_ = [
        ("year", ctypes.c_int), ("month", ctypes.c_int), ("day", ctypes.c_int),
        ("hour", ctypes.c_int), ("minute", ctypes.c_int), ("second", ctypes.c_int),
        ("timezone", ctypes.c_int),
    ]


SET_TIMEOUT_CB = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_int)
READ_CB = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t))
WRITE_CB = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t))
CLOSE_CB = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)


class dc_custom_cbs_t(ctypes.Structure):
    _fields_ = [
        ("set_timeout", SET_TIMEOUT_CB), ("set_break", ctypes.c_void_p),
        ("set_dtr", ctypes.c_void_p), ("set_rts", ctypes.c_void_p),
        ("get_lines", ctypes.c_void_p), ("get_available", ctypes.c_void_p),
        ("configure", ctypes.c_void_p), ("poll", ctypes.c_void_p),
        ("read", READ_CB), ("write", WRITE_CB),
        ("ioctl", ctypes.c_void_p), ("flush", ctypes.c_void_p),
        ("purge", ctypes.c_void_p), ("sleep", ctypes.c_void_p),
        ("close", CLOSE_CB),
    ]


DIVE_CB = ctypes.CFUNCTYPE(
    ctypes.c_int,
    ctypes.POINTER(ctypes.c_ubyte), ctypes.c_uint,
    ctypes.POINTER(ctypes.c_ubyte), ctypes.c_uint,
    ctypes.c_void_p,
)


def main():
    rx_queue = queue.Queue()
    timeout_ms = [3500]
    loop = asyncio.new_event_loop()
    client_holder = []
    ready_evt = threading.Event()
    stop_evt = asyncio.Event()

    def ble_worker():
        asyncio.set_event_loop(loop)

        async def runner():
            print(f"Connecting to {DEVICE_MAC} on {HCI_ADAPTER}...")
            async with BleakClient(DEVICE_MAC, bluez={"adapter": HCI_ADAPTER}) as client:
                client_holder.append(client)
                await client.start_notify(VENDOR_NOTIFY_UUID, lambda s, d: rx_queue.put(bytes(d)))
                ready_evt.set()
                await stop_evt.wait()
                await client.stop_notify(VENDOR_NOTIFY_UUID)

        loop.run_until_complete(runner())

    t = threading.Thread(target=ble_worker, daemon=True)
    t.start()
    if not ready_evt.wait(timeout=20.0):
        print("Timed out waiting for BLE connection.")
        return

    @SET_TIMEOUT_CB
    def cb_set_timeout(userdata, timeout):
        timeout_ms[0] = timeout if timeout > 0 else 3500
        return DC_STATUS_SUCCESS

    @READ_CB
    def cb_read(userdata, data_ptr, size, actual_ptr):
        try:
            pkt = rx_queue.get(timeout=timeout_ms[0] / 1000.0)
            n = min(len(pkt), size)
            ctypes.memmove(data_ptr, pkt, n)
            if actual_ptr:
                actual_ptr[0] = n
            return DC_STATUS_SUCCESS
        except queue.Empty:
            if actual_ptr:
                actual_ptr[0] = 0
            return DC_STATUS_TIMEOUT

    @WRITE_CB
    def cb_write(userdata, data_ptr, size, actual_ptr):
        payload = ctypes.string_at(data_ptr, size)
        fut = asyncio.run_coroutine_threadsafe(
            client_holder[0].write_gatt_char(VENDOR_WRITE_UUID, payload, response=True), loop
        )
        fut.result(timeout=5.0)
        if actual_ptr:
            actual_ptr[0] = size
        return DC_STATUS_SUCCESS

    @CLOSE_CB
    def cb_close(userdata):
        return DC_STATUS_SUCCESS

    ctx = ctypes.c_void_p()
    libdc.dc_context_new(ctypes.byref(ctx))

    it = ctypes.c_void_p()
    libdc.dc_descriptor_iterator(ctypes.byref(it))
    desc = ctypes.c_void_p()
    libdc.dc_descriptor_get_vendor.restype = ctypes.c_char_p
    libdc.dc_descriptor_get_product.restype = ctypes.c_char_p

    target_desc = ctypes.c_void_p()
    while libdc.dc_iterator_next(it, ctypes.byref(desc)) == DC_STATUS_SUCCESS:
        if libdc.dc_descriptor_get_vendor(desc) == b"Sunroad" and libdc.dc_descriptor_get_product(desc) == b"D3":
            target_desc = ctypes.c_void_p(desc.value)
            break
    libdc.dc_iterator_free(it)

    cbs = dc_custom_cbs_t()
    cbs.set_timeout = cb_set_timeout
    cbs.read = cb_read
    cbs.write = cb_write
    cbs.close = cb_close

    iostream = ctypes.c_void_p()
    libdc.dc_custom_open(ctypes.byref(iostream), ctx, DC_TRANSPORT_BLE, ctypes.byref(cbs), None)

    device = ctypes.c_void_p()
    libdc.dc_device_open(ctypes.byref(device), ctx, target_desc, iostream)

    dive_idx = [0]

    @DIVE_CB
    def on_dive(data_ptr, size, fp_ptr, fp_size, userdata):
        dive_idx[0] += 1
        # Save first dive blob so it can also be inspected with `dctool parse`
        if dive_idx[0] == 1:
            raw_blob = ctypes.string_at(data_ptr, size)
            open("/tmp/sunroad_live_dive1.bin", "wb").write(raw_blob)

        parser = ctypes.c_void_p()
        if libdc.dc_parser_new2(ctypes.byref(parser), ctx, target_desc, data_ptr, size) == DC_STATUS_SUCCESS:
            dt = dc_datetime_t()
            libdc.dc_parser_get_datetime(parser, ctypes.byref(dt))
            divetime = ctypes.c_uint()
            maxdepth = ctypes.c_double()
            avgdepth = ctypes.c_double()
            mintemp = ctypes.c_double()
            libdc.dc_parser_get_field(parser, DC_FIELD_DIVETIME, 0, ctypes.byref(divetime))
            libdc.dc_parser_get_field(parser, DC_FIELD_MAXDEPTH, 0, ctypes.byref(maxdepth))
            libdc.dc_parser_get_field(parser, DC_FIELD_AVGDEPTH, 0, ctypes.byref(avgdepth))
            rc_t = libdc.dc_parser_get_field(parser, DC_FIELD_TEMPERATURE_MINIMUM, 0, ctypes.byref(mintemp))
            t_str = f"{mintemp.value:.0f}°C" if rc_t == DC_STATUS_SUCCESS else "N/A"
            print(
                f"[libdivecomputer Dive #{dive_idx[0]:02d}] "
                f"{dt.year:04d}-{dt.month:02d}-{dt.day:02d} {dt.hour:02d}:{dt.minute:02d}:{dt.second:02d} | "
                f"Dur: {divetime.value:2d}s | Max: {maxdepth.value:4.1f}m | Avg: {avgdepth.value:4.1f}m | "
                f"Temp: {t_str} | Blob: {size}B"
            )
            libdc.dc_parser_destroy(parser)
        return 1

    print("Running dc_device_foreach() via libdivecomputer.so...")
    libdc.dc_device_foreach(device, on_dive, None)

    libdc.dc_device_close(device)
    libdc.dc_iostream_close(iostream)
    libdc.dc_context_free(ctx)
    loop.call_soon_threadsafe(stop_evt.set)
    t.join(timeout=3.0)


if __name__ == "__main__":
    main()
