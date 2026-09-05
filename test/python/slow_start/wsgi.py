import os
import time

# Widen the window between the worker's PROCESS_CREATED and its PROCESS_READY,
# which is what test_python_prototype_killed_mid_start() kills the prototype
# inside of.  The delay is at import time, before `application` is even
# defined: nxt_python_start() imports the module and only then sends READY
# (src/python/nxt_python.c), so a sleep here lands squarely in that window,
# while a sleep inside application() would not -- the process is ready long
# before a request arrives.
time.sleep(float(os.environ.get('UNIT_SLOW_START', '4')))


def application(environ, start_response):
    start_response('200 OK', [('Content-Length', '0')])
    return []
