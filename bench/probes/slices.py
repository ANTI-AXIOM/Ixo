def run(n):
    text = "the quick brown fox jumps over the lazy dog " * 2000
    parts = []
    for i in range(n):
        k = i % 80_000
        parts.append(text[k:k + 12])
    total = 0
    for p in parts:
        total += len(p)
    print(len(parts), total)
run(500_000)
