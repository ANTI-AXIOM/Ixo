import re
def run(n):
    lines = [f"user{i}@host{i % 50}.example.com sent {i * 3} bytes" for i in range(n)]
    hits = 0
    total = 0
    for l in lines:
        if re.search(r"@host1\d\.", l):
            hits += 1
        total += len(re.findall(r"\d+", l))
    print(hits, total)
run(50_000)
