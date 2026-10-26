class Body
    # The file does not exist, so the module fails to send the body.
    def to_path
        '/nonexistent/unit-test-body'
    end

    def close
        $closed += 1
    end
end

$closed = 0

app = Proc.new do |env|
    if env['PATH_INFO'] == '/closed'
        ['200', {'X-Closed' => $closed.to_s, 'Content-Length' => '0'}, []]
    else
        ['200', {}, Body.new]
    end
end

run app
