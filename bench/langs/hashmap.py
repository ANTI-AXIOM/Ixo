# counting with int keys
def run(n):
    counts = {}
    for i in range(n):
        k = (i * 7919) % 100_003
        counts[k] = counts.get(k, 0) + 1
    print(len(counts), counts[0], counts[5])


run(3_000_000)
