# Chapter 18: Networking and Data Formats

Modern applications rarely exist in isolation. They need to communicate with web services, parse data formats, and exchange information. NAAb provides a set of modules to handle networking and data serialization efficiently.

## 18.1 The `http` Module

The `http` module provides a client for making HTTP requests. It supports standard methods like GET, POST, PUT, and DELETE.

```naab
use http

main {
    // Basic GET request
    let response = http.get("https://api.example.com/data")
    
    if response["ok"] {
        print("Status:", response["status"])
        print("Body:", response["body"])
    } else {
        print("Request failed:", response["status"])
    }

    // POST request with JSON data
    let payload = {"name": "NAAb User", "role": "admin"}
    let post_resp = http.post(
        "https://api.example.com/users", 
        payload, 
        {"Content-Type": "application/json"}
    )
    
    print("Post result:", post_resp["status"])
}
```

### 18.1.1 Response Structure

The `http` functions return a dictionary with the following structure:
*   `status`: Integer HTTP status code (e.g., 200, 404).
*   `body`: String containing the response body.
*   `headers`: Dictionary of response headers.
*   `ok`: Boolean, true if status is 2xx.

## 18.2 The `json` Module

JSON (JavaScript Object Notation) is the lingua franca of the web. The `json` module allows you to parse JSON strings into NAAb data structures (dicts and arrays) and serialize NAAb data back into JSON strings.

```naab
use json

main {
    // Parsing JSON
    let json_str = "{\"name\": \"Alice\", \"scores\": [10, 20, 30]}"
    let data = json.parse(json_str)
    
    print("Name:", data["name"])
    print("First score:", data["scores"][0])

    // Serializing to JSON
    let my_data = {
        "id": 123,
        "active": true,
        "tags": ["user", "new"]
    }
    let output_str = json.stringify(my_data)
    print("Serialized:", output_str)
    
    // Pretty printing
    let pretty_str = json.pretty(my_data)
    print(pretty_str)
}
```

## 18.3 The `csv` Module

For processing tabular data, the `csv` module provides functions to read and write CSV files.

```naab
use csv

main {
    // Parse a CSV string
    let csv_content = "name,age,role\nAlice,30,Engineer\nBob,25,Designer"
    let rows = csv.parse(csv_content)
    
    // Result is a list of dictionaries (if headers present)
    print("First row:", rows[0]) 
    // Output: {"name": "Alice", "age": "30", "role": "Engineer"}
    
    // Write data to CSV string
    let new_csv = csv.stringify(rows)
    print(new_csv)
}
```