# functions passed around and called through a variable
def apply_all(f, n):
    t = 0
    for i in range(n):
        t += f(i)
    return t


def run(n):
    k = 3
    add_k = lambda x: x + k
    sq = lambda x: x * x % 1000
    print(apply_all(add_k, n), apply_all(sq, n))


run(5_000_000)
