import requests
from bs4 import BeautifulSoup
import json

url = "https://www.vibecodingtools.tech/agents?page=2"
extraction_rules = {
    "item_container": "div.relative.group",
    "agent_name": "h3",
    "agent_description": "p.text-gray-600"
}

scraped_items_list = []
error_message = None

try:
    print(f"Fetching URL: {url}")
    response = requests.get(url, timeout=10)
    response.raise_for_status()
    print(f"Response status: {response.status_code}")
    soup = BeautifulSoup(response.text, 'html.parser')

    item_container_selector = extraction_rules.get("item_container", "")
    
    if item_container_selector:
        containers = soup.select(item_container_selector)
        print(f"Found {len(containers)} containers matching '{item_container_selector}'")
        for container in containers:
            name_el = container.select_one(extraction_rules.get("agent_name", ""))
            desc_el = container.select_one(extraction_rules.get("agent_description", ""))
            
            name = name_el.get_text(strip=True) if name_el else "N/A"
            description = desc_el.get_text(strip=True) if desc_el else "N/A"
            
            scraped_items_list.append({
                "name": name,
                "description": description,
                "raw_html_snippet": str(container)[:100] + "..." # Truncate for brevity
            })
    else:
        print("No item_container_selector provided.")

except requests.exceptions.RequestException as e:
    error_message = f"Python Request Error: {e}"
except Exception as e:
    error_message = f"Python Parsing/General Error: {e}"

if error_message:
    print(f"Error: {error_message}")
else:
    print(f"Scraped {len(scraped_items_list)} items.")
    if len(scraped_items_list) > 0:
        print("Sample item:", json.dumps(scraped_items_list[0], indent=2))