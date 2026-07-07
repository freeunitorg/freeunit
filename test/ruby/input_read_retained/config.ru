# Retain the rack.input handle from the first request in a global, then read
# it during a later request handled by the same worker. Each handle is bound to
# its originating request (rctx->req_seq snapshot), so a stale handle must
# yield nil instead of the current request's body -- otherwise a client could
# read another client's request body on a shared worker (cross-request
# disclosure). Regression guard for fix b10a68b4.
$saved_input = nil

app = Proc.new do |env|
    if $saved_input.nil?
        # First request: stash the handle without consuming it.
        $saved_input = env['rack.input']
        out = 'stashed'
    else
        # Second request: read the retained handle from the first request
        # before touching this request's own input. A request-bound handle
        # returns nil here; only a leaky one would expose this body.
        leaked =
            begin
                $saved_input.read.to_s
            rescue StandardError => e
                "err:#{e.class}"
            end
        out = "leaked[#{leaked}]"
    end

    [200, { 'Content-Length' => out.bytesize.to_s }, [out]]
end

run app
