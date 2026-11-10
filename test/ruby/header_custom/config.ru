app = Proc.new do |env|
    headers = {
        'Content-Length' => '0',
        'Custom-Header' => env['rack.input'].read
    }
    if env['HTTP_X_LONG_NAME']
        headers['X-' + ('a' * 254)] = '1'
    end
    ['200', headers, []]
end

run app
