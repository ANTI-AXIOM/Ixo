# hash map counting with string keys
def run(n):
    counts = {}
    x = 42
    for i in range(n):
        x = (x * 1103515245 + 12345) % 2147483648
        key = f"k{x % 5000}"
        counts[key] = counts.get(key, 0) + 1
    print(len(counts), max(counts.values()), counts["k7"])


run(1_000_000)
