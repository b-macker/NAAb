const items = Array.from({length: 10000}, (_, i) => ({id: i, name: `item_${i}`, tags: ['t0','t1','t2','t3','t4'], value: i * 2.718}));
const start = Date.now();
const result = JSON.stringify(items);
console.log((Date.now() - start) * 1000);
