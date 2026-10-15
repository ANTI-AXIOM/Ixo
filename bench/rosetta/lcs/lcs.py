# Length of the longest common subsequence of two DNA strings (dynamic programming).
def dna(seed, n):
    x, out = seed, []
    for _ in range(n):
        x = (x * 1103515245 + 12345) % 2_147_483_648
        out.append("ACGT"[(x >> 16) & 3])
    return "".join(out)


a, b = dna(1, 1500), dna(2, 1500)
prev = [0] * (len(b) + 1)
for ca in a:
    row = [0]
    for j, cb in enumerate(b):
        row.append(prev[j] + 1 if ca == cb else max(prev[j + 1], row[j]))
    prev = row
print(prev[-1])
