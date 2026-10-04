from concurrent.futures import ProcessPoolExecutor
def work(k):
    s = 0
    for i in range(200_000):
        s += (i * k) % 7
    return s
if __name__ == "__main__":
    with ProcessPoolExecutor(4) as ex:
        print(sum(ex.map(work, range(8))))
