# A command-line tool: count the words given as arguments and print the most common.
import argparse
from collections import Counter

parser = argparse.ArgumentParser(description="Count words and print the most common ones.")
parser.add_argument("words", nargs="+")
parser.add_argument("--top", type=int, default=5)
parser.add_argument("--upper", action="store_true")
args = parser.parse_args()
counts = Counter(w.upper() if args.upper else w for w in args.words)
width = max(len(w) for w in counts)
for w, n in counts.most_common(args.top):
    print(f"{w:<{width}} {n:>3} {'#' * n}")
