# legacy_enterprise.py
import json, math, hashlib, time

def process_monolith(raw_data):
    # 1. Parsing
    items = json.loads(raw_data)
    
    # 2. Math (The Bottleneck)
    total = 0.0
    for item in items:
        val = item['amt']
        for _ in range(100): val = math.sqrt(val * val + 0.01)
        total += val
        
    # 3. Security
    payload = f"TOTAL:{total}".encode()
    integrity = hashlib.sha256(payload).hexdigest()
    
    # 4. Reporting
    return {
        "final_value": total,
        "hash": integrity,
        "engine": "Legacy Python"
    }

if __name__ == "__main__":
    test = json.dumps([{"id": 1, "amt": 100.5}, {"id": 2, "amt": 200.75}])
    print(json.dumps(process_monolith(test)))
