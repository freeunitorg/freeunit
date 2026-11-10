
require('http').createServer(function (req, res) {
    var length = parseInt(req.headers['x-length'] || '300', 10);

    if (!req.headers['x-catch']) {
        res.setHeader('X'.repeat(length), 'value');
        res.end('body');
        return;
    }

    try {
        res.setHeader('X'.repeat(length), 'value');

    } catch (e) {
        res.statusCode = 500;
        res.end(e.message);
        return;
    }

    res.end('body');
}).listen(8080);
