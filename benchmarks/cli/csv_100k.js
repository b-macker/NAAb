const lines = Array.from({length: 100000}, (_, i) =>
    `${i},name_${i},${(i*3.14).toFixed(2)},${i%2===0?'active':'inactive'}`
).join('\n');
const start = Date.now();
const rows = lines.split('\n').map(l => l.split(','));
console.log((Date.now() - start) * 1000);
