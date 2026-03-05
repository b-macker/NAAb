const data = Array.from({length: 10000}, () => Math.floor(Math.random() * 1000000));
const start = Date.now();
data.sort((a, b) => a - b);
const end = Date.now();
console.log((end - start) * 1000);
