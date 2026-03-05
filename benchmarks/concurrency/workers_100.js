// Node.js single-threaded — simulates with promises
const start = Date.now();
const workers = Array.from({length: 100}, (_, idx) =>
    new Promise(resolve => {
        let s = 0;
        for (let i = 0; i < 10000; i++) s += i;
        resolve(s);
    })
);
Promise.all(workers).then(() => console.log((Date.now() - start) * 1000));
