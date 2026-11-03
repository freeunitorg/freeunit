"""Decompressors shared by the compression tests.

zstd needs two shapes: one complete frame, and the next bytes of a body that
is still streaming.  ``compression.zstd`` arrives with Python 3.14; older
interpreters use the ``zstandard`` package, and a test skips when neither is
installed.
"""

import pytest


def zstd_decompress(data):
    """Decodes one complete zstd frame, or skips the test."""

    try:
        from compression import zstd  # Python 3.14 and later.

        return zstd.decompress(data)
    except ImportError:
        pass

    zstandard = pytest.importorskip(
        'zstandard', reason='python zstd bindings not installed'
    )

    # A frame written without a pledged size carries no content size, so the
    # one-shot decompress() cannot be used.  decompressobj() reads the frame
    # header first.
    d = zstandard.ZstdDecompressor().decompressobj()
    out = d.decompress(data)

    if not d.eof:
        raise ValueError('zstd frame is not complete')

    return out


def zstd_stream_decoder():
    """Returns a decoder for the next bytes of a zstd body, or skips."""

    try:
        from compression import zstd  # Python 3.14 and later.

        return zstd.ZstdDecompressor().decompress
    except ImportError:
        pass

    zstandard = pytest.importorskip(
        'zstandard', reason='python zstd bindings not installed'
    )

    return zstandard.ZstdDecompressor().decompressobj().decompress


def brotli_decompress(data):
    """Decodes one complete brotli stream, or skips the test."""

    brotli = pytest.importorskip(
        'brotli', reason='python brotli bindings not installed'
    )

    return brotli.decompress(data)
