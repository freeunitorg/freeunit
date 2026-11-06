
require('http').createServer(function (req, res) {
    var length = parseInt(req.headers['x-length'] || '300', 10);

    res.setHeader('X'.repeat(length), 'value');
    res.end('body');
}).listen(8080);
