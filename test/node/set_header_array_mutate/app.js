
require('http').createServer(function (req, res) {
    let cookies = ['a=1'];

    res.setHeader('Set-Cookie', cookies);

    // setHeader() keeps a copy, so these changes do not reach the response.
    cookies[0] = 'b=' + 'x'.repeat(64);
    cookies.push('c=3');

    res.end('body');
}).listen(8080);
