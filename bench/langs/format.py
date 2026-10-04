# number to text formatting and parsing back
def run(n):
    total = 0
    back = 0.0
    for i in range(n):
        s = f"{i}:{i * 0.5:.2f}|{i % 13}"
        total += len(s)
        back += float(s.split("|")[0].split(":")[1])
    print(total, back)


run(300_000)
