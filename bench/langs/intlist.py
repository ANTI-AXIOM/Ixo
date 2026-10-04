# a big list of ints kept in memory
def run(n):
    xs = []
    for i in range(n):
        xs.append(i * 7919 % 1_000_003)
    total = 0
    big = 0
    for x in xs:
        total += x
        if x > big:
            big = x
    print(len(xs), total, big)


run(5_000_000)
