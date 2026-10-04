def make(k):
    return lambda x: x + k
def run(n):
    fs = []
    for i in range(n):
        fs.append(make(i))
    s = 0
    for f in fs:
        s += f(1)
    print(len(fs), s)
run(500_000)
