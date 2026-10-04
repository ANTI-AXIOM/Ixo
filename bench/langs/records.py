# many small objects kept alive, each with a string and a short list
class Rec:
    def __init__(self, id, name, score, tags):
        self.id = id
        self.name = name
        self.score = score
        self.tags = tags


def run(n):
    recs = []
    for i in range(n):
        recs.append(Rec(i, f"name{i}", i % 100, [i % 7, i % 11]))
    total = 0
    long_names = 0
    for r in recs:
        total += r.score + r.tags[0] + r.tags[1]
        if len(r.name) > 9:
            long_names += 1
    print(len(recs), total, long_names)


run(300_000)
