#!/usr/bin/env python3

#
# Simple stopwatch console app.
#
# Crank up the terminal font size, point the camera at the screen, and display
# the camera output on the same desktop. Taking a screenshot of the desktop, or
# taking a photo of the monitor with a phone, then gives a rough idea of the
# accumulated latency due to camera capture, video encoding, packetization,
# encryption, decryption, depacketization, video decoding, rendering, display,
# etc. but hopefully excluding network latency.
#

import sys
import time

FPS = 30

i = 0
start_time = time.time()
while True:
    i += 1

    sys.stdout.write('\n {}\n'.format(i))
    sys.stdout.flush()

    now = time.time() - start_time
    if now > 10.0:
        start_time = time.time()
        now = 0.0

    t = '{:.6f}'.format(now)
    t = '\n ' + ' '.join([*t]) + '\n'
    sys.stdout.write(t)
    sys.stdout.flush()

    time.sleep(1.0/FPS)

    sys.stdout.write('\033[2J\033[H')
    sys.stdout.flush()
