const start = Date.now();
let sum = 0;
for (let i = 0; i < 10000; i++) sum += i;
console.log((Date.now() - start) * 1000);
