const http = require('http');
const server = http.createServer((req, res) => { res.end('ok'); });
server.listen(0, '127.0.0.1', () => {
    const port = server.address().port;
    let done = 0;
    const start = Date.now();
    for (let i = 0; i < 100; i++) {
        http.get(`http://127.0.0.1:${port}/`, (res) => {
            let d = '';
            res.on('data', c => d += c);
            res.on('end', () => {
                done++;
                if (done === 100) {
                    console.log((Date.now() - start) * 1000);
                    server.close();
                }
            });
        });
    }
});
