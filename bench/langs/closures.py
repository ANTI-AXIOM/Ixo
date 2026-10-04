# map / filter / reduce pipelines and calling closures
def compose(f, g):
    return lambda x: g(f(x))


def run(n):
    nums = list(range(n))
    evens = [y for y in (x * 3 for x in nums) if y % 2 == 0]
    s = sum(evens)
    big = sum(1 for x in nums if x > n // 2)
    step = compose(lambda x: x + 1, lambda x: x * 2)
    t = 0
    for x in nums:
        t += step(x)
    print(len(evens), s, big, t)


run(1_000_000)
