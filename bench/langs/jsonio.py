# encode and decode JSON records
import json


def run(n):
    records = []
    for i in range(n):
        records.append({"id": i, "name": f"user{i}", "tags": ["a", "b", f"t{i % 7}"], "score": i * 0.25 + 0.125, "ok": i % 3 == 0})
    text = json.dumps(records, separators=(",", ":"))
    back = json.loads(text)
    ids = 0
    oks = 0
    for r in back:
        ids += r["id"]
        if r["ok"]:
            oks += 1
    print(len(text), len(back), ids, oks)


run(100_000)
