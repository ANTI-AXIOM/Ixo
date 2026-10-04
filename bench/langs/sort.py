# sorting numbers, sorting objects by a key, custom comparator
import functools


class Item:
    def __init__(self, key, name):
        self.key = key
        self.name = name


def run(n):
    x = 7
    nums = []
    for i in range(n):
        x = (x * 1103515245 + 12345) % 2147483648
        nums.append(x % 1_000_000)
    a = sorted(nums)
    items = [Item(k, "i") for k in nums[0:n // 4]]
    b = sorted(items, key=lambda it: it.key)
    c = sorted(nums[0:n // 4], key=functools.cmp_to_key(lambda p, q: q - p))
    print(a[0], a[n // 2], a[n - 1], b[0].key, b[len(b) - 1].key, c[0])


run(500_000)
