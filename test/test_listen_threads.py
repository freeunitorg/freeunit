import time
from pathlib import Path

import pytest
from conftest import _count_fds, pid_by_name

from unit.applications.proto import ApplicationProto

client = ApplicationProto()

BODY = '0123456789'

# Enough engines that shrinking destroys several of them on any host.  The
# leak is per destroyed engine, so a cycle that starts from the default
# (nxt_ncpu) would destroy nothing on a single-CPU machine; pinning the high
# water mark here makes the delta the same everywhere.
THREADS = 4


@pytest.fixture(autouse=True)
def setup_method_fixture(temp_dir):
    Path(f'{temp_dir}/assets').mkdir(parents=True)
    Path(f'{temp_dir}/assets/index.html').write_text(BODY, encoding='utf-8')

    assert 'success' in client.conf(
        {
            "listeners": {"*:8080": {"pass": "routes"}},
            "routes": [{"action": {"share": f'{temp_dir}/assets$uri'}}],
        }
    )

    yield

    # The no-restart suite preserves /settings between tests.
    assert 'success' in client.conf_delete('settings/listen_threads')


def _router_fds():
    return _count_fds(pid_by_name('unit: router'))


def _settle_fds(stable=5, timeout=100):
    # Engines are created and destroyed asynchronously with respect to the
    # configuration reply, so a reading taken right after it can catch the
    # router mid-transition.  Wait for the count to hold still instead of
    # sleeping a fixed amount.
    fds = _router_fds()
    same = 0

    for _ in range(timeout):
        time.sleep(0.1)

        cur = _router_fds()

        same = same + 1 if cur == fds else 0
        fds = cur

        if same >= stable:
            break

    return fds


def _wait_fds(expected, timeout=100):
    # Return the last reading either way, so a leak fails as the count it is
    # rather than as an opaque timeout.
    for _ in range(timeout):
        fds = _router_fds()

        if fds <= expected:
            break

        time.sleep(0.1)

    return fds


def _set_threads(threads):
    assert 'success' in client.conf(
        {"listen_threads": threads}, 'settings'
    ), f'listen_threads {threads}'

    assert client.get()['status'] == 200, f'request with {threads} engines'


def test_listen_threads_engine_fds():
    # A destroyed router engine used to keep its port socketpair and queue
    # memfd open for the router's lifetime (3 descriptors each), because
    # nxt_router_thread_exit_handler() only dropped one of the port's two
    # references and nxt_port_release() closes nothing.  Every shrink of
    # "listen_threads" therefore cost descriptors that no later grow
    # returned.
    _set_threads(THREADS)

    before = _settle_fds()

    _set_threads(1)
    _set_threads(THREADS)

    after = _wait_fds(before)

    assert after <= before, (
        f'router leaked {after - before} descriptors over a listen_threads'
        f' {THREADS} -> 1 -> {THREADS} cycle ({before} before, {after} after)'
    )


def _set_threads_no_listeners(threads):
    return client.conf({"listen_threads": threads}, 'settings')


def test_listen_threads_shrink_grow_no_listeners():
    # nxt_router_engine_quit() allocates its nxt_joint_job_t (the nxt_work_t
    # *and* the nxt_task_t the work item points at) from tmcf->mem_pool but
    # used to take no tmcf->count reference, so nxt_router_conf_ready() could
    # release the pool while the job was still sitting on the dying engine's
    # locked work queue.  The window is widest with no listeners configured:
    # engine->joints is then empty, so nxt_router_worker_thread_quit() is the
    # only thing standing between the post and nxt_thread_exit(), and there is
    # no listen-socket delete job on the same queue to serialise behind.
    #
    # The following grow -> shrink -> grow cycle failed 11 of 15 runs of this
    # test on master (14 of 15 against an ASan build, which reports the
    # heap-use-after-free directly) and 0 of 15 with the reference taken.
    # Each PUT is asserted, so a "Value doesn't exist." reply is never
    # counted as a clean cycle, and the router pid is compared every cycle
    # because the control API only reports the failed apply of the crashed
    # generation, not the crash itself.
    assert 'success' in client.conf(
        {
            "listeners": {},
            "routes": [{"action": {"return": 200}}],
        }
    )

    pid = pid_by_name('unit: router')

    for i in range(50):
        for threads in (8, 4, 8):
            assert 'success' in _set_threads_no_listeners(
                threads
            ), f'cycle {i}: listen_threads {threads}'

        assert pid_by_name('unit: router') == pid, (
            f'router restarted during cycle {i}: the engine-quit job was'
            ' freed with tmcf->mem_pool before the dying engine drained it'
        )
