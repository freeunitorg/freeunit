import re


def check_scheduler(output_version):
    # "unitd --version" prints the ./configure options of the build.  The
    # "schedules" configuration object exists only with --scheduler.
    return re.search('--scheduler', output_version) is not None
