# Polyglot Composition Patterns

Design patterns for building high-performance multi-language NAAb scripts.

## Overview

This guide presents 15 proven patterns for composing multiple languages in NAAb scripts. Each pattern shows optimal language selection for specific workflow types.

**Pattern Categories:**
1. Data Processing Pipelines
2. Web Services & APIs
3. Systems Programming
4. Scientific Computing
5. DevOps & Automation

---

## Pattern 1: ETL Pipeline

**Use Case:** Extract, Transform, Load data from multiple sources

**Languages:** Shell → Python → Julia → Nim

**Pattern:**
```naab
main {
    // EXTRACT: Shell for file discovery
    let csv_files = <<shell
find /data -name "*.csv" -type f -mtime -7 | sort
    >>

    // TRANSFORM: Python for data cleaning
    let cleaned = <<python
import pandas as pd
dfs = [pd.read_csv(f) for f in csv_files.split('\n')]
combined = pd.concat(dfs)
combined.dropna().to_dict('records')
    >>

    // ANALYZE: Julia for statistical processing
    let statistics = <<julia
using Statistics, DataFrames
df = DataFrame(cleaned)
describe(df)
    >>

    // LOAD: Nim for fast database insertion
    let inserted = <<nim
import db_sqlite, json
let db = open("output.db", "", "", "")
for row in cleaned:
    db.exec(sql"INSERT INTO data VALUES (?, ?)", row)
    >>
}
```

**Why This Works:**
- Shell: 2-3x faster for file operations
- Python: Rich data manipulation libraries
- Julia: 10-100x faster for statistics
- Nim: Fast compiled DB operations

**Performance:** 5-10x faster than pure Python

---

## Pattern 2: Web Scraper

**Use Case:** Scrape websites, extract data, process results

**Languages:** JavaScript → Python → Shell

**Pattern:**
```naab
main {
    // FETCH: JavaScript for web requests
    let pages = <<javascript
const urls = ['url1', 'url2', 'url3'];
const fetches = urls.map(url => fetch(url).then(r => r.text()));
await Promise.all(fetches);
    >>

    // PARSE: Python for HTML extraction
    let data = <<python
from bs4 import BeautifulSoup
results = []
for html in pages:
    soup = BeautifulSoup(html, 'html.parser')
    results.extend([{
        'title': link.text,
        'url': link.get('href')
    } for link in soup.find_all('a')])
json.dumps(results)
    >>

    // FILTER: Shell for URL validation
    let valid_urls = <<bash
echo "$data" | jq -r '.[].url' | grep -E '^https://' | sort -u
    >>

    // DOWNLOAD: JavaScript for concurrent fetching
    let downloads = <<javascript
const downloads = valid_urls.split('\n').map(async url => {
    const response = await fetch(url);
    return {url, content: await response.text()};
});
await Promise.all(downloads);
    >>
}
```

**Benefits:**
- JavaScript: Native async/await, fetch API
- Python: BeautifulSoup for HTML parsing
- Shell: Fast text filtering
- Concurrent downloads: 10x faster than sequential

---

## Pattern 3: Scientific Computing Pipeline

**Use Case:** High-performance numerical analysis

**Languages:** Python → Julia → Nim

**Pattern:**
```naab
main {
    // LOAD: Python for data ingestion
    let data = <<python
import numpy as np
np.load('dataset.npy').tolist()
    >>

    // COMPUTE: Julia for heavy math
    let results = <<julia
using LinearAlgebra, Statistics

# Convert to Julia array
data_matrix = hcat(data...)

# Fast linear algebra
U, S, V = svd(data_matrix)
eigenvalues = eigvals(data_matrix' * data_matrix)

# Statistical analysis
stats = (
    mean=mean(data_matrix),
    std=std(data_matrix),
    correlation=cor(data_matrix)
)

# Return results
(U=U, S=S, V=V, eigenvalues=eigenvalues, stats=stats)
    >>

    // VISUALIZE: Python for plotting
    <<python
import matplotlib.pyplot as plt
plt.plot(results['eigenvalues'])
plt.savefig('eigenvalues.png')
    >>

    // EXPORT: Nim for fast serialization
    let saved = <<nim
import json, strutils
let output = %* results
writeFile("results.json", $output)
    >>
}
```

**Performance Gain:** 50-100x faster than pure Python for linear algebra

---

## Pattern 4: Real-Time Data Processing

**Use Case:** Process streaming data with low latency

**Languages:** Go → Julia → Rust

**Pattern:**
```naab
main {
    // INGEST: Go for concurrent data collection
    let stream_data = <<go
package main
import "sync"

func main() {
    ch := make(chan []float64, 1000)
    var wg sync.WaitGroup

    // Spawn workers
    for i := 0; i < 10; i++ {
        wg.Add(1)
        go worker(ch, &wg)
    }

    // Collect results
    wg.Wait()
}
    >>

    // ANALYZE: Julia for real-time statistics
    let analysis = <<julia
using OnlineStats
o = Series(Mean(), Variance())
for batch in stream_data
    fit!(o, batch)
end
value(o)
    >>

    // PERSIST: Rust for lock-free storage
    let stored = <<rust
use crossbeam::channel::unbounded;
let (tx, rx) = unbounded();
// Lock-free concurrent writes
    >>
}
```

**Key Features:**
- Go: Lightweight goroutines for concurrency
- Julia: Fast online statistical algorithms
- Rust: Thread-safe data structures

---

## Pattern 5: Systems Monitoring Tool

**Use Case:** Monitor system resources, aggregate metrics

**Languages:** Shell → Zig → Python

**Pattern:**
```naab
main {
    // COLLECT: Shell for system info
    let metrics = <<bash
echo "CPU: $(top -bn1 | grep 'Cpu(s)' | awk '{print $2}')"
echo "MEM: $(free -m | awk 'NR==2{print $3}')"
echo "DISK: $(df -h / | awk 'NR==2{print $5}')"
    >>

    // PROCESS: Zig for fast parsing
    let parsed = <<zig
const std = @import("std");
// Parse metrics with zero-copy
    >>

    // AGGREGATE: Python for time-series
    let aggregated = <<python
import pandas as pd
df = pd.DataFrame(parsed)
df.resample('1min').mean()
    >>

    // ALERT: Go for notifications
    <<go
if parsed.CPU > 80 {
    sendAlert("High CPU usage")
}
    >>
}
```

---

## Pattern 6: Machine Learning Inference

**Use Case:** Load model, preprocess data, run inference

**Languages:** Python → Julia → C++

**Pattern:**
```naab
main {
    // LOAD MODEL: Python for ML frameworks
    let model = <<python
import torch
model = torch.load('model.pt')
model.eval()
model
    >>

    // PREPROCESS: Julia for fast data transformation
    let preprocessed = <<julia
using Statistics
# Normalize data 10x faster than Python
data_normalized = (data .- mean(data)) ./ std(data)
    >>

    // INFERENCE: C++ for production speed
    let predictions = <<cpp
#include <torch/torch.h>
// Load model and run inference
// 5-10x faster than Python
    >>

    // POSTPROCESS: Python for result formatting
    <<python
import json
results = json.dumps({
    'predictions': predictions,
    'confidence': confidence_scores
})
    >>
}
```

**Performance:** 10-50x faster inference than pure Python

---

## Pattern 7: API Gateway

**Use Case:** Route requests, transform data, aggregate responses

**Languages:** Go → JavaScript → Python

**Pattern:**
```naab
main {
    // ROUTING: Go for high-throughput server
    let router = <<go
package main
import "net/http"

func main() {
    http.HandleFunc("/", handler)
    http.ListenAndServe(":8080", nil)
}
    >>

    // TRANSFORM: JavaScript for JSON manipulation
    let transformed = <<javascript
const data = JSON.parse(request.body);
const enriched = {
    ...data,
    timestamp: new Date().toISOString(),
    version: 'v2'
};
JSON.stringify(enriched);
    >>

    // BACKEND: Python for business logic
    let response = <<python
import requests
# Call multiple microservices
responses = [requests.get(url) for url in service_urls]
# Aggregate results
    >>
}
```

---

## Pattern 8: Image Processing Pipeline

**Use Case:** Process images at scale

**Languages:** C++ → Python → Nim

**Pattern:**
```naab
main {
    // DECODE: C++ for fast image decoding
    let images = <<cpp
#include <opencv2/opencv.hpp>
cv::Mat img = cv::imread("input.jpg");
// 10x faster than Python PIL
    >>

    // FILTER: C++ for convolution operations
    let filtered = <<cpp
cv::GaussianBlur(img, output, cv::Size(5,5), 0);
cv::Canny(output, edges, 50, 150);
    >>

    // DETECT: Python for ML inference
    let detections = <<python
import tensorflow as tf
model = tf.saved_model.load('detector')
detections = model(image_tensor)
    >>

    // ANNOTATE: Nim for fast drawing
    let annotated = <<nim
import nimPNG
# Draw bounding boxes 5x faster
    >>
}
```

**Performance:** 20-50x faster than pure Python

---

## Pattern 9: Log Analysis

**Use Case:** Parse logs, extract patterns, generate reports

**Languages:** Shell → Python → SQL

**Pattern:**
```naab
main {
    // GREP: Shell for fast text filtering
    let errors = <<bash
grep "ERROR" /var/log/*.log | \
    awk '{print $1, $2, $5}' | \
    sort | uniq -c | sort -rn
    >>

    // PARSE: Python for structured extraction
    let parsed = <<python
import re
pattern = r'(\d+) (\S+) (\S+) (\w+)'
matches = [re.match(pattern, line) for line in errors.split('\n')]
structured = [m.groups() for m in matches if m]
    >>

    // QUERY: SQL for aggregation
    let summary = <<sql
SELECT error_type, COUNT(*) as count, MAX(timestamp) as last_seen
FROM errors
GROUP BY error_type
ORDER BY count DESC
LIMIT 10
    >>
}
```

---

## Pattern 10: Distributed Task Queue

**Use Case:** Distribute work across multiple workers

**Languages:** Go → Python → Rust

**Pattern:**
```naab
main {
    // QUEUE: Go for task distribution
    let queue = <<go
package main
import "github.com/gocraft/work"

pool := work.NewWorkerPool(MyContext{}, 10, "namespace", redisPool)
pool.Job("send_email", (*MyContext).SendEmail)
pool.Start()
    >>

    // WORKER: Python for task execution
    let worker = <<python
def process_task(task):
    # Execute business logic
    return result
    >>

    // RESULTS: Rust for lock-free aggregation
    let aggregated = <<rust
use crossbeam::channel::unbounded;
let (tx, rx) = unbounded();
// Collect results without locks
    >>
}
```

---

## Pattern 11: Build System

**Use Case:** Compile multiple languages, run tests

**Languages:** Shell → Multiple

**Pattern:**
```naab
main {
    // BUILD C++
    <<bash
g++ -O3 -o output src/*.cpp
    >>

    // BUILD Rust
    <<bash
cd rust_project && cargo build --release
    >>

    // RUN TESTS: Go
    <<go
package main
import "testing"
// Run Go tests
    >>

    // BENCHMARK: Julia
    <<julia
using BenchmarkTools
@benchmark my_function()
    >>

    // REPORT: Python
    <<python
import junitxml
# Generate test report
    >>
}
```

---

## Pattern 12: Cryptocurrency Trading Bot

**Use Case:** Real-time market data, fast execution

**Languages:** Rust → JavaScript → Python

**Pattern:**
```naab
main {
    // MARKET DATA: Rust for low-latency WebSocket
    let prices = <<rust
use tokio_tungstenite::connect_async;
// Ultra-low latency price feed
    >>

    // STRATEGY: Julia for quantitative analysis
    let signals = <<julia
using Statistics, TimeSeries
# Fast technical indicators
sma = mean(prices)
signal = if current > sma "BUY" else "SELL"
    >>

    // EXECUTION: JavaScript for exchange API
    let order = <<javascript
const response = await fetch('https://api.exchange.com/order', {
    method: 'POST',
    body: JSON.stringify({type: signal, amount: 1.0})
});
    >>

    // LOGGING: Python for data persistence
    <<python
import logging
logging.info(f"Executed {signal} at {price}")
    >>
}
```

---

## Pattern 13: Video Transcoding

**Use Case:** Convert video formats at scale

**Languages:** Shell → C++ → Python

**Pattern:**
```naab
main {
    // DECODE: Shell + FFmpeg
    <<bash
ffmpeg -i input.mp4 -f rawvideo -pix_fmt rgb24 - | ...
    >>

    // PROCESS: C++ for frame manipulation
    <<cpp
#include <opencv2/opencv.hpp>
// Process frames 100x faster than Python
    >>

    // ENCODE: Shell + FFmpeg
    <<bash
ffmpeg -f rawvideo -pix_fmt rgb24 -s 1920x1080 -i - output.mp4
    >>

    // METADATA: Python for MP4 tagging
    <<python
from mutagen.mp4 import MP4
video = MP4("output.mp4")
video["\xa9nam"] = "Processed Video"
video.save()
    >>
}
```

---

## Pattern 14: Database Migration

**Use Case:** Transform and migrate data between systems

**Languages:** Shell → SQL → Python

**Pattern:**
```naab
main {
    // EXPORT: Shell + psql
    let old_data = <<bash
psql -d old_db -c "COPY users TO STDOUT WITH CSV HEADER"
    >>

    // TRANSFORM: Python for data cleaning
    let transformed = <<python
import csv, io
reader = csv.DictReader(io.StringIO(old_data))
cleaned = [{
    'id': row['old_id'],
    'name': row['name'].strip(),
    'email': row['email'].lower()
} for row in reader]
    >>

    // LOAD: SQL for bulk insert
    <<sql
COPY new_users FROM STDIN WITH (FORMAT csv, HEADER true);
${transformed}
\.
    >>
}
```

---

## Pattern 15: Configuration Generator

**Use Case:** Generate config files for multiple systems

**Languages:** Python → YAML → Shell

**Pattern:**
```naab
main {
    // TEMPLATE: Python for logic
    let configs = <<python
services = ['api', 'worker', 'scheduler']
configs = {
    service: {
        'replicas': 3 if service == 'api' else 1,
        'port': 8000 + i
    }
    for i, service in enumerate(services)
}
    >>

    // RENDER: Python for YAML generation
    let yaml_configs = <<python
import yaml
{name: yaml.dump(config) for name, config in configs.items()}
    >>

    // DEPLOY: Shell for kubectl
    <<bash
for config in *.yaml; do
    kubectl apply -f $config
done
    >>
}
```

---

## Anti-Patterns to Avoid

### ❌ Anti-Pattern 1: Single Language Everything

```naab
// BAD: Python for everything
main {
    let result = <<python
# File operations (shell 2-3x faster)
# Heavy math (Julia 10-100x faster)
# Systems calls (Zig safer)
# All in Python!
    >>
}
```

### ❌ Anti-Pattern 2: Too Many Language Switches

```naab
// BAD: Excessive switching overhead
main {
    let a = <<python "hello" >>
    let b = <<julia uppercase(a) >>  // Serialization cost
    let c = <<nim len(b) >>          // Another serialization
    let d = <<go fmt.Println(c) >>   // Yet another
}
```

**Better:** Group operations by language

### ❌ Anti-Pattern 3: Using Wrong Language for Task

```naab
// BAD: JavaScript for numerical computation
let eigenvalues = <<javascript
// Slow, limited precision
>>

// GOOD: Julia for numerical computation
let eigenvalues = <<julia
using LinearAlgebra
eigvals(matrix)
>>
```

---

## Summary Checklist

When designing multi-language scripts:

- [ ] Each language chosen for its strengths
- [ ] Language transitions minimized
- [ ] Performance-critical paths optimized
- [ ] Data serialization overhead considered
- [ ] Team skills factored in
- [ ] Maintenance complexity balanced with performance

---

## References

- [Task→Language Matrix](task_language_matrix.md) - Language scoring reference
- [Optimization Guide](optimization_guide.md) - Configuration and tuning
- [AI Polyglot Guidance](../AI_POLYGLOT_GUIDANCE.md) - AI assistant guide
