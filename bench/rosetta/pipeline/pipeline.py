# A pipeline over queues: a producer, four worker threads, a consumer that sums.
import queue
import threading

jobs, results = queue.Queue(256), queue.Queue(256)
DONE = None


def produce():
    for i in range(1, 100_001):
        jobs.put(i)
    for _ in range(4):
        jobs.put(DONE)


def work():
    while (n := jobs.get()) is not DONE:
        results.put(n * n % 1000)
    results.put(DONE)


threading.Thread(target=produce).start()
for _ in range(4):
    threading.Thread(target=work).start()
total, finished = 0, 0
while finished < 4:
    r = results.get()
    if r is DONE:
        finished += 1
    else:
        total += r
print(total)
