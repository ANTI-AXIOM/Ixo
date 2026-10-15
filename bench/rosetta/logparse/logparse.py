# Requests per status code and the three slowest endpoints, from access log lines.
paths = ["/", "/login", "/api/items", "/api/orders", "/static/app.js"]
log = [f"10.0.0.{i % 50} GET {paths[i % 5]} {[200, 200, 404, 500, 302][i * 3 % 5]} {i * 37 % 900}ms" for i in range(50_000)]
by_status, total, seen = {}, {}, {}
for line in log:
    _, _, path, status, ms = line.split()
    by_status[status] = by_status.get(status, 0) + 1
    total[path] = total.get(path, 0) + int(ms[:-2])
    seen[path] = seen.get(path, 0) + 1
for s in sorted(by_status):
    print(s, by_status[s])
for p in sorted(total, key=lambda p: -total[p] / seen[p])[:3]:
    print(p, f"{total[p] / seen[p]:.1f}")
