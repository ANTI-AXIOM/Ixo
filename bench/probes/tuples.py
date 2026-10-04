def run(n):
    ps = []
    for i in range(n):
        ps.append((i, i * 2))
    s = 0
    for a, b in ps:
        s += a + b
    print(len(ps), s)
run(1_000_000)
