
require('http').createServer(function (req, res) {
    if (req.url == '/first') {
        res.setHeader('Set-Cookie', []);
        res.setHeader('X-Header', '1');

    } else if (req.url == '/last') {
        res.setHeader('X-Header', '1');
        res.setHeader('Set-Cookie', []);

    } else if (req.url == '/override') {
        res.setHeader('Set-Cookie', ['a=1', 'b=2']);
        res.setHeader('Set-Cookie', []);

    } else if (req.url == '/two') {
        res.setHeader('Set-Cookie', []);
        res.setHeader('Set-Cookie', ['a=1', 'b=2']);
        res.setHeader('X-Empty', []);

    } else {
        res.setHeader('Set-Cookie', []);
    }

    res.end('body');
}).listen(8080);
