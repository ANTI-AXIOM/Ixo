# whole-list sums, minimums, maximums and searches
def run(n):
    xs = [i * 7919 % 1_000_003 for i in range(n)]
    fs = [i * 0.5 for i in range(n)]
    t = 0
    for k in range(20):
        t += sum(xs) + max(xs) - min(xs)
        if -k in xs:
            t += 1
    print(t, sum(fs), xs.index(999_999))


run(5_000_000)
