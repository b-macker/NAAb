const net = require('net');
const server = net.createServer(socket => { socket.pipe(socket); });
server.listen(0, '127.0.0.1', () => {
    const port = server.address().port;
    const client = net.createConnection(port, '127.0.0.1', () => {
        const msg = JSON.stringify({id: 1, data: "hello"});
        let count = 0;
        const start = Date.now();
        client.on('data', () => {
            count++;
            if (count < 1000) {
                client.write(msg);
            } else {
                console.log((Date.now() - start) * 1000);
                client.end();
                server.close();
            }
        });
        client.write(msg);
    });
});
