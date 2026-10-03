"""Open the profiler's vendor bulk device and drop leftovers from an interrupted run."""
import sys

import usb.core
import usb.util


def open_device():
    dev = usb.core.find(idVendor=0x303A, idProduct=0x4020)
    if dev is None:
        sys.exit("device 303a:4020 not found")
    intf = dev.get_active_configuration()[(0, 0)]
    def ep(direction):
        return usb.util.find_descriptor(intf, custom_match=lambda e: usb.util.endpoint_direction(e.bEndpointAddress) == direction)
    ep_out, ep_in = ep(usb.util.ENDPOINT_OUT), ep(usb.util.ENDPOINT_IN)
    try:
        while True:
            ep_in.read(64 * 1024, 200)
    except usb.core.USBTimeoutError:
        pass
    return ep_out, ep_in
