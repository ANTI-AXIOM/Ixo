# prime sieve: list writes and reads
def run(n):
    flags = [True] * (n + 1)
    flags[0] = False
    flags[1] = False
    i = 2
    while i * i <= n:
        if flags[i]:
            j = i * i
            while j <= n:
                flags[j] = False
                j += i
        i += 1
    count = 0
    for f in flags:
        if f:
            count += 1
    return count


print(run(2_000_000))
