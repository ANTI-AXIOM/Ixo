# integer arithmetic in a loop
def run(n):
    total = 0
    for i in range(n):
        total += (i * i + 3 * i) % 7
    return total


print(run(10_000_000))
