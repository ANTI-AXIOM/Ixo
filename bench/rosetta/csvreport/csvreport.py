# Sales by region from CSV text: totals, the best product of each region, sorted.
import csv
import io
from collections import defaultdict

lines = ["region,product,units,price"]
for i in range(20_000):
    lines.append(f"{['north', 'south', 'east', 'west'][i % 4]},p{i % 13},{i % 7 + 1},{i % 11 + 0.5}")
rows = list(csv.DictReader(io.StringIO("\n".join(lines))))
regions = defaultdict(list)
for r in rows:
    regions[r["region"]].append(r)
for region in sorted(regions):
    sales = regions[region]
    total = sum(int(r["units"]) * float(r["price"]) for r in sales)
    units = defaultdict(int)
    for r in sales:
        units[r["product"]] += int(r["units"])
    best = max(units, key=units.get)
    print(region, f"{total:.2f}", best)
