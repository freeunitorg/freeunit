from unit.applications.lang.wasm import ApplicationWasm


def check_clang_wasm():
    return ApplicationWasm.prepare_env('hello') is not None
