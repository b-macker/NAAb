const data = JSON.stringify(Array.from({length: 20000}, (_, i) => ({id: i, name: `item_${i}`, value: i * 3.14})));
const start = Date.now();
const parsed = JSON.parse(data);
console.log((Date.now() - start) * 1000);
