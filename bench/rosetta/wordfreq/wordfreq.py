# The five most frequent words of a text.
from collections import Counter

words = "the quick brown fox jumps over the lazy dog and the cat sat on the mat".split()
text = " ".join(words[i * 7 % len(words)] for i in range(200_000))
counts = Counter(text.split())
for w, n in sorted(counts.items(), key=lambda p: (-p[1], p[0]))[:5]:
    print(w, n)
