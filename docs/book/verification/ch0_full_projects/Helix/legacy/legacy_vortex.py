# legacy_vortex.py
import json, math, hashlib, time

def parse_and_clean(raw_json):
    # Targets GO (json.loads, join)
    data = json.loads(raw_json)
    cleaned = [str(x).strip().upper() for x in data if x]
    return "|".join(cleaned)

def heavy_calculation(val):
    # Targets CPP (sqrt, pow)
    res = math.sqrt(math.pow(val, 2) + 0.5)
    for _ in range(100):
        res = math.sqrt(res + 0.01)
    return res

def secure_signature(data_str):
    # Targets RUST (sha256)
    return hashlib.sha256(data_str.encode()).hexdigest()

def format_output(id_val, result):
    # Targets RUBY (format)
    return "ID: {:04d} | Result: {:.4f}".format(int(id_val), result)

def process_vortex(raw_input):
    # Entry point for validator
    data = json.loads(raw_input) # List of [id, val]
    total = 0.0
    for item in data:
        h = heavy_calculation(item[1])
        total += h
    
    return {
        "total_value": total,
        "status": "VORTEX_PROCESSED"
    }

if __name__ == "__main__":
    test = json.dumps([[1, 10.5], [2, 20.0]])
    print(json.dumps(process_vortex(test)))
