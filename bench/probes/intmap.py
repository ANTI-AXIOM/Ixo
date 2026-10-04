def run(n):
    m = {}
    for i in range(n):
        m[i * 7] = i
    st = {0}
    for i in range(n):
        st.add(i * 3)
    hits = 0
    for i in range(n):
        if i in m:
            hits += 1
    print(len(m), len(st), hits)
run(1_000_000)
