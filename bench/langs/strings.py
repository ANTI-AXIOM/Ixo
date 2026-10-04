# string building, join, upper case, split, search
def run(n):
    words = []
    for i in range(n):
        words.append(f"word{i % 997}x")
    text = " ".join(words)
    parts = text.upper().split(" ")
    hits = sum(1 for p in parts if "7" in p)
    total = 0
    for p in parts:
        total += len(p)
    built = ""
    for i in range(n // 10):
        built += parts[i][0:2]
    print(len(parts), hits, total, len(built))


run(300_000)
