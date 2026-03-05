const lines = Array.from({length: 100000}, (_, i) => `The quick brown fox jumps over the lazy dog #${i}`);
const pattern = /\b\w{5,}\b/g;
const start = Date.now();
let count = 0;
for (const line of lines) { count += (line.match(pattern) || []).length; }
console.log((Date.now() - start) * 1000);
