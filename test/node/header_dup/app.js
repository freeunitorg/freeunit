require('http').createServer(function (req, res) {
    let body = JSON.stringify({headers: req.headers, raw: req.rawHeaders});

    res.writeHead(200, {'Content-Length': Buffer.byteLength(body)});
    res.end(body);
}).listen(8080);
