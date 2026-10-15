# Orders as JSON to a per-customer summary as JSON.
import json

orders = [{"id": i, "customer": f"c{i % 37}", "items": [{"sku": f"s{j}", "qty": j + 1} for j in range(i % 4 + 1)]} for i in range(5000)]
text = json.dumps(orders)
summary = {}
for o in json.loads(text):
    s = summary.setdefault(o["customer"], {"orders": 0, "units": 0})
    s["orders"] += 1
    s["units"] += sum(item["qty"] for item in o["items"])
out = json.dumps([{"customer": c, **summary[c]} for c in sorted(summary)], separators=(",", ":"))
print(len(out), out[:120])
