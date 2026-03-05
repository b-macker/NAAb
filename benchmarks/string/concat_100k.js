const parts = Array.from({length: 100000}, (_, i) => String(i));
const start = Date.now();
const result = parts.join('');
console.log((Date.now() - start) * 1000);
