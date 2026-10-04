def run(n):
    s = "abcdefghij" * (n // 10)
    count = 0
    for i in range(len(s)):
        if s[i] == "e":
            count += 1
    print(len(s), count)
run(200_000)
