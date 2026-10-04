# a big map with string keys
def run(n):
    m = {}
    for i in range(n):
        m[f"key{i}"] = i
    hits = 0
    for i in range(0, n, 3):
        if f"key{i}" in m:
            hits += 1
    print(len(m), hits, m["key12345"])


run(500_000)
