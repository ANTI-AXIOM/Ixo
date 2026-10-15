# The Ixo language guide

This guide describes the language as the `ixo` interpreter implements it. Every example
compiles; `tests/tools/doc_check.py` checks that.

Three symbols carry most of the syntax: `=` means "is" (`fn f(x) = x * 2`), `:` means "has
type" (`x: int`, `fn f(x): int`) and `->` means "then" (`if ok -> go`, `match` arms, one-line
loops). Code written for Ixo 0.5 still runs, with a warning for each old spelling;
[Upgrading from 0.5](#upgrading-from-05) explains how to update it in one command.

- [Running programs](#running-programs)
- [Basics](#basics)
- [Variables and scope](#variables-and-scope)
- [Strings](#strings)
- [Control flow](#control-flow)
- [Functions](#functions)
- [Lambdas and the universal underscore](#lambdas-and-the-universal-underscore)
- [Collections](#collections)
- [Classes](#classes)
- [Operator methods](#operator-methods)
- [Interfaces](#interfaces)
- [Enums](#enums)
- [Pattern matching](#pattern-matching)
- [Errors](#errors)
- [Cleanup with defer](#cleanup-with-defer)
- [Types](#types)
- [Modules](#modules)
- [Packages and building](#packages-and-building)
- [Concurrency](#concurrency)
- [Parallelism](#parallelism)
- [GPU compute](#gpu-compute)
- [Memory regions](#memory-regions)
- [More of the standard library](#more-of-the-standard-library)
- [Performance](#performance)
- [Tools](#tools)
- [Upgrading from 0.5](#upgrading-from-05)

## Running programs

A file runs from top to bottom; there's no required `main`. If the file defines a `fn main`
that takes no arguments, it runs after the top-level code.

```ixo
print("Hello")          # runs first

fn main
    print("from main")  # runs second
```

`ixo file.ixo arg1 arg2` makes the extra arguments available as `args` (a list of strings).

## Basics

Comments start with `#`. A string on its own line, often `"""..."""`, is a docstring and has no
effect.

| value | examples | notes |
|---|---|---|
| `int` | `42`, `-7`, `1_000_000`, `0xff`, `0b1010` | 64-bit, wraps on overflow |
| `float` | `3.14`, `1e-3` | 64-bit IEEE |
| `bool` | `true`, `false` | |
| `nil` | `nil` | "no value" |
| `str` | `"text"`, `'text'`, `"""multi-line"""`, `f"{x}"`, `r"C:\raw"` | immutable, UTF-8 |
| `list` | `[1, 2, 3]` | growable |
| `map` | `{"a": 1}`, `{}` | keeps insertion order |
| `set` | `{1, 2}`, `set()` | |
| `tuple` | `(1, "a")`, `(1,)`, `()` | immutable |
| `range` | `0..10` (without 10), `1..=5` (with 5), `0..100 by 5` | integers |

Durations are ints in milliseconds: `500ms` is `500` and `2s` is `2000`.

Operators, from loosest to tightest:

| operators | meaning |
|---|---|
| `:>` | send to a flow |
| `!` | error handler (`expr ! err -> fallback`) |
| `or`, `and`, `not` | logic; `and`/`or` return one of their operands |
| `==` `!=` `<` `<=` `>` `>=` `is` `is not` `in` `not in` | comparison; `==` compares contents, `is` identity (or a type test: `p is Point`) |
| `..` `..=` `by` | ranges: `..` leaves out the end, `..=` includes it; `by` sets the step |
| `\|` `^` `&` `<<` `>>` | bitwise on ints; union / symmetric difference / intersection on sets |
| `+` `-` | add, subtract, concatenate strings, lists, tuples, maps |
| `*` `/` `%` `//` | `/` always gives a float; `%` has the sign of the divisor; `//` divides and rounds down (`7 // 2` is `3`, `-7 // 2` is `-4`) |
| `-x` `~x` | negate, bitwise not |
| `**` | power |
| `x.f` `x(...)` `x[i]` `x?` `&f` | member, call, index, error propagation, a function itself |

Unary minus binds looser than `**` and `.`, as in maths: `-2 ** 2` is `-4` and `-5.abs` is
`-5` (write `(-5).abs` for 5). Roots are methods: `x.sqrt`, `27.root(3)`.

Only `nil`, `false` and errors are false in conditions. `0`, `""` and `[]` are true; check
them with `x == 0`, `s.is_empty` or `list.is_empty`.

## Variables and scope

Assignment creates a variable; there is no declaration keyword. `x: int = 3` declares a typed
variable.

```ixo
count = 0
name = "Ixo"
a, b = 1, 2
a, b = b, a        # swap
count += 1
count++            # also --, -=, *=, /=, %=, **=
```

The scoping rules are:

1. `x = ...` at the top level of a file creates a **module variable**.
2. `x = ...` inside a function creates a **local of that function**, visible in the whole
   function, unless an enclosing function already has `x`. In that case it updates that
   variable, which is how closures change their captured state.
3. **Compound assignment** (`+=`, `++`, ...) never creates a variable. It updates the nearest
   existing one, module variables included.
4. Loop variables, pattern bindings and `! err` names only exist inside their block.

```ixo
score = 0
fn add_points(n)
    score += n        # updates the module variable

fn reset_local
    score = 0         # a new local; the module variable is unchanged
    score

fn make_counter
    count = 0
    fn next()         # with (): `next` is the function, next() calls it
        count += 1    # updates make_counter's count
        count
    return next
```

Reading a variable before it has a value is a runtime error.

## Strings

```ixo
s = "Hello, World"
print(s.len, s.up, s.down)            # 12 HELLO, WORLD hello, world
print(s[0], s[-1], s[0..5])          # H d Hello
print(s.cut(", "), "a b".cut)         # ["Hello", "World"] ["a", "b"]
print(", ".join(["x", "y"]))          # x, y
print(s.replace("l", "L"), "ab" * 3)  # HeLLo, WorLd ababab
print("42".int + 1)                   # 43
```

Indexes and lengths count characters (code points), not bytes. Escapes: `\n \t \r \\ \" \'
\0 \xHH \u{HHHH}`. In an f-string, `{{` and `}}` are literal braces.

A raw string, `r"..."` (also `r'...'` and `r"""..."""`), keeps backslashes as they are,
which suits Windows paths and regular expressions: `r"C:\temp\new"`, `re.test(r"\d+", s)`.

F-strings interpolate any expression. An optional format goes after `:`:

```ixo
name = "Ixo"
price = 1234.5678
print(f"{name} costs {price:.2f}")    # Ixo costs 1234.57
print(f"[{name:>6}] [{name:<6}] [{name:^7}]")
print(f"{42:05} {1234567:,} {0.25:.1%} {255:x} {{literal braces}}")
```

The format spec is `[[fill]align][sign][0][width][,][.precision][type]`. Align is `<`, `>` or
`^`; type is `f`, `e`, `g`, `d`, `x`, `b`, `%` or `s`.

## Control flow

Blocks are indented (4 spaces is the convention). A single statement can follow `->` on
the same line instead; after `else`, it follows directly.

```ixo
x = 7
if x > 10
    print("big")
elif x > 5 -> print("medium")
else print("small")

while x > 0
    x -= 3

for i in 0..3 -> print(i)            # 0 1 2
for i in 10..=0 by -5 -> print(i)    # 10 5 0
for c in "hé" -> print(c)
for i, item in ["a", "b"] -> print(i, item)        # the position and the item
for key, value in {"a": 1} -> print(key, value)    # the key and the value
for (name, n) in [("x", 1), ("y", 2)] -> print(name, n)   # parentheses unpack each item
for i, (name, n) in [("x", 1)] -> print(i, name, n)        # both
for _ in 1..=3 -> print("again")      # _ ignores the value
```

`break` and `continue` work in `for` and `while` loops. Two names give the position (the key
for maps) and the item, without making a pair for each item; parentheses unpack items that are
tuples or lists.

`if` and `match` are expressions. Their value is the last expression in the chosen block,
or `nil` if nothing was chosen.

```ixo
x = 5
size = if x > 3 -> "big" else "small"
grade = if x > 8 -> "A"
        elif x > 4 -> "B"
        else "C"
```

Comparisons chain as in maths: `0 <= i < n` is `0 <= i and i < n`, with `i` written once.
The middle of a chain is used twice, so it is a name, a number, a field, an index or
arithmetic on those.

## Functions

```ixo
fn greet(name)
    "Hello, " + name              # the last expression is returned

fn clamp(value, lo, hi)
    if value < lo -> return lo
    if value > hi -> return hi
    value

fn area(w: float, h: float): float = w * h    # one-line body with =
fn describe(x, prefix = "value") = f"{prefix}: {x}"

fn tick                                          # declared without parentheses
    print("tick")
fn version = "1.0"
```

Calling:

- **Parentheses follow the declaration.** A function declared without a parameter list
  (`fn tick`, `fn version = "1.0"`) is called wherever its name is used: `tick` on its own
  line, `print(version)`, `player.alive`. A function declared with parentheses, even `fn f()`,
  is only called with them; its name alone is the function (`f = greet`, `items.each(show)`).
  `&tick` gives a function declared without parentheses as a value. Built-in methods that
  need no arguments work the same way: `list.len`, `16.sqrt`, `s.up`.
- **Any function can be called like a method.** `x.f(a)` means `f(x, a)` when `x` has no
  method `f`, and `x.f` means `f(x)`, so `5.double`, `name.greet` and `items.print` all work.
- **Named arguments** pass values by parameter name, after the positional ones:
  `save("a.txt", backup: false)`. Parameters you skip keep their defaults. They work with
  functions, methods, lambdas and classes (`Window("main", height: 100)`), and the checker
  reports unknown or repeated names. Built-in functions take positional arguments only.
- Functions are values: pass them, store them, return them.
- Functions defined at the top level of a file can be called before their definition.
- A string on the first line of a function's body is its **docstring**. Editors show it in
  hover, completion and signature help. `#` comment lines directly above a declaration work
  too:

```ixo
fn save(path, overwrite = false, backup = true)
    """Writes the document to `path`.

    With `backup`, the old file is kept as path.bak."""
    f"{path} {overwrite} {backup}"

# The area of a w by h rectangle.
fn area(w, h) = w * h

print(save("a.txt", backup: false))
```

**Unpacking parameters.** A parameter written in parentheses unpacks the argument:

```ixo
fn dist((x1, y1), (x2, y2)) = ((x2 - x1) ** 2 + (y2 - y1) ** 2).sqrt
print(dist((0, 0), (3, 4)), [(1, 2), (3, 4)].map(fn((a, b)) = a * b))   # 5.0 [2, 12]
```

**`main` and the command line.** When the program's main file has a `fn main`, it runs after
the file's top-level code. With parameters, the command line fills them:

- a parameter without a default is a value given in order;
- one with a default is an option: `--top 3` or `--top=3`;
- a `bool` is a flag: `--upper` (and `--no-upper`);
- a `list` takes the remaining values.

Values convert to the parameter's type (from its annotation or its default). A wrong or
missing value prints the usage and exits with 2, and `--help` prints the usage with main's
docstring. A program that calls `main(...)` itself keeps doing so.

```ixo-fragment
fn main(words: list[str], top = 5, upper = false)
    "Counts words and prints the most common ones."
    counts = words.map(if upper -> _.up else _).counts
    for (w, n) in counts.pairs.sort_by(-_[1])[..top] -> print(w, n)
```

`ixo run count.ixo --top 2 apple kiwi apple` prints `apple 2` and `kiwi 1`.

## Lambdas and the universal underscore

```ixo
double = fn(x) = x * 2
add = fn(a, b = 1) = a + b
greet = fn()
    print("hello")                # a lambda with a block body
```

In a call argument, `_` turns the argument into a lambda: `list.map(_ * 2)` is
`list.map(fn(x) = x * 2)`. When the receiver is a list, range, tuple, string or set, `map`,
`keep`, `skip`, `each`, `fold`, `sum_by`, `count`, `any`, `all` and `find_by` with a lambda run
it inside the loop itself, without creating a function or calling it for each item.

- `_` is the single argument. Every `_` in the lambda is the same value.
- Named placeholders `_a`, `_b`, `_key`, ... give several arguments, **in alphabetical order**:
  `sort(_a > _b)` sorts descending.
- The lambda forms at the argument of the nearest **method call** that contains the
  placeholder (`.map`, `.keep`, `.each`, ...). Plain calls inside it are part of the lambda:
  `data.map(Point(_["x"], _["y"]))` builds a point per item. Without an enclosing method
  call, it forms at the nearest call argument: `apply(_ * 3, 7)`.
- `->` after a method call starts a trailing lambda. Inside it, placeholders always belong
  to it:

```ixo
nums = [5, 3, 8]
nums.each -> print(_ + 1)
squares = nums.map -> _ * _
total = nums.fold(0, -> _a + _b)
nums.each ->
    print("item", _)
    print("square", _ * _)
```

Placeholder lambdas ignore extra arguments, so `map.each(print(_a, _b))` and
`map.each(print(_))` both work.

**Blocks inside brackets.** A lambda body, `match` arms or an `if`/`else` block can be
written inside a call or a list: end the line with the block's header (`fn(x)`, `match x`,
`if cond`, `else`) and indent the block. The block ends when a line goes back to the
header's indentation, or at the closing bracket:

```ixo
items = [1, 2, 3]
labels = items.map(fn(n)
    match n
        1 -> "one"
        2 -> "two"
        _ -> "many"
)
print(match labels.len
    3 -> "three labels"
    _ -> "other")
```

## Collections

```ixo
l = [3, 1, 2]
l.add(4)                     # also: insert(i, x), drop(i), remove(x), take/pop, wipe
print(l[0], l[-1], l[1..3], l.get(10, "default"))
print(l.sort, l.sort(_a > _b), l.rev, l.sum, l.min, l.uniq)
l.sort!                      # in place: `!` marks the twin of a method that makes a copy
print(l.map(_ * 2).keep(_ > 2).fold(0, _a + _b))
print(l.any(_ > 3), l.all(_ > 0), l.count(_ > 1), l.find_by(_ > 1))
print(l.group_by(_ % 2), l.partition(_ > 2), l.sort_by(-_))

m = {"apples": 3}
m["pears"] = 5
print(m.get("kiwis", 0), "apples" in m, m.keys, m.values, m.pairs)
m.drop("apples")
m.each(print(_a, _b))

s = {1, 2, 3}
s.add(4)
print(2 in s, s | {9}, s & {1, 2}, s - {1})

t = (1, "two", 3.0)
x, y, z = t
print(t[1], t.len)
```

Slices take a range, with either end left open, and an optional step after `by`. They work
on lists, strings and tuples:

```ixo
xs = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]
print(xs[7..], xs[..3], xs[-3..])        # [7, 8, 9] [0, 1, 2] [7, 8, 9]
print(xs[0..10 by 2], xs[.. by -1])          # [0, 2, 4, 6, 8] [9, 8, 7, 6, 5, 4, 3, 2, 1, 0]
print("hello"[.. by -1])                   # olleh
```

`sort`, `rev`, `uniq` and `mix` make a new list; `sort!`, `rev!`, `uniq!` and `mix!` change
this one. Methods that only change it (`add`, `drop`, `take`, `wipe`) have plain names. Using
`l.sort` as a statement, where the copy is lost, is a warning. `m[i, j]` is `m[(i, j)]`: a
matrix element, or a map with tuple keys.

`list(x)`, `set(x)`, `tuple(x)` and `map(pairs)` convert. Lists, maps and sets can't be map
keys or set elements; use tuples. The [standard library reference](stdlib.md) lists every
method.

More helpers:

- `_` skips a value when unpacking: `ip, _, path, _ = line.cut`.
- `m[k] += v` on a map starts from an empty value when the key is missing (0 for numbers,
  `""` for strings, `[]` for lists): `for w in words -> counts[w] += 1`.
- `xs.counts` says how many times each item occurs, as a map in first-seen order.
- `heap.heap_push(x)` and `heap.heap_pop` keep a list as a min-heap, a priority queue like
  Python's `heapq`: `heap_pop` gives the smallest item.

```ixo
counts = {}
for w in "a b a c a".cut -> counts[w] += 1
heap = []
for v in [5, 1, 4] -> heap.heap_push(v)
print(counts, "a b a".cut.counts, heap.heap_pop)   # {"a": 3, "b": 1, "c": 1} {"a": 2, "b": 1} 1
```

### Numeric arrays

`xs.array` makes a numeric array from numbers (so do `array(n)`, `array(n, fill)` and
`array(items)`). Arithmetic and comparisons on an array go item by item, and a plain number
stands for every item. Everything a list does, an array does too.

```ixo
xs = [1.0, 2.0, 3.0, 4.0].array
ys = xs * 2.0 + 1                                  # array([3.0, 5.0, 7.0, 9.0])
print(ys, xs[xs > 2.0], (xs - 10).abs.max, xs.mean, xs.dot(xs))
i = (0..5).array
print(i[i % 2 == 0], i // 2, i == (0..5).array)    # array([0, 2, 4]) array([0, 0, 1, 1, 2]) true
```

- `+ - * / // % **` and `< <= > >=` work item by item. Against a number, `==` and `!=` give
  a mask too (`xs[xs == 0]`); two arrays are equal or not as wholes.
- `xs[mask]` keeps the items where a bool array is true; `xs[positions]` picks items.
- Methods: `abs`, `sqrt`, `exp`, `ln`, `sin`, `cos`, `floor`, `ceil`, `round`,
  `clamp(lo, hi)`, `mean`, `dot(ys)`, and `.list` for a plain list again.
- Large arrays (a million items and more) are worked on by all cores.

Arrays are their own type because `[1, 2] * 3` repeats a list: arithmetic item by item on
lists would make that ambiguous.

## Classes

```ixo
class Point(x, y)                     # constructor parameters become fields
    fn len = (x ** 2 + y ** 2).sqrt   # fields by their bare names
    fn add(other) = Point(x + other.x, y + other.y)
    fn str = f"({x}, {y})"            # used by print and str()

class Player
    hp = 100                          # a field with a default (evaluated for each instance)
    items = []
    fn init(name)                     # init runs after the fields are set
        self.name = name              # init may add fields
    fn hit(damage: int)
        hp -= damage                  # changes the field
        if not alive -> print(f"{name} is down")
    fn alive = hp > 0

p = Point(3, 4)
print(p.len, p.add(Point(1, 1)))      # 5.0 (4, 5)
hero = Player("Ann")
hero.hit(30)
print(hero.hp, hero.alive)            # 70 true
```

- Inside a method, a field or method is used by its **bare name** (`hp`, `alive`), and
  assigning a field's name assigns the field. Parameters and locals come first; `self.x`
  always works. A method doesn't hide a built-in (`json`, `print`): call it as `self.json`.
- Fields come from the constructor parameters, the class body and `self.x = ...` in `init`,
  and nowhere else. The layout is fixed when the class is compiled, so a field access is a
  direct slot and a misspelled field is a compile error.
- `obj.method` calls a method declared without parentheses (`fn alive`); for one declared with
  them, `obj.method` is the bound method (`h = hero.hit`, then `h(10)`).
- Methods are **private to their module** unless marked with `*`: `fn area*`. Fields are
  public.
- Without a `str` method, instances print as `Player(hp: 70, items: [], name: "Ann")`, and
  instances without fields print as the class name.
- `x is Point` is true when `x` is an instance of `Point` (it also works with interfaces and
  enums).

### Data records

`data` declares an immutable record in one line; its fields are the names in parentheses.

```ixo
data Point(x: float, y: float)
    fn len = (x * x + y * y).sqrt

p = Point(3.0, 4.0)
q = p.with(y: 0.0)                       # a copy with some fields changed
print(p, q.len, p == Point(3.0, 4.0))    # Point(x: 3.0, y: 4.0) 3.0 true
print({p: "seen"}[Point(3.0, 4.0)], [p, q].sort.first)
```

- Two records are equal when their fields are. They hash by their fields, so they work as
  map keys and in sets, and they sort field by field.
- Their fields don't change: `p.x = 1.0` is a compile error (a panic through `any`).
  `p.with(x: 1.0)` makes the changed copy.
- Patterns match them as classes: `Point(x, 0.0) -> ...`.
- Methods may follow, indented, as in a class. A record has no other fields.

## Operator methods

A class can give meaning to operators by defining methods named after them:

```ixo
class Vec2(x, y)
    fn +(o) = Vec2(x + o.x, y + o.y)
    fn -(o) = Vec2(x - o.x, y - o.y)
    fn -() = Vec2(-x, -y)                       # unary minus
    fn *(k) = Vec2(x * k, y * k)
    fn ==(o) = x == o.x and y == o.y
    fn <(o) = x < o.x                           # also gives > <= >= and sorting
    fn str = f"<{x}, {y}>"

class Grid
    cells = {}
    fn [](p) = cells.get(p, 0)                  # g[p]
    fn []=(p, v)                                # g[p] = v
        cells[p] = v

class Bag
    items = []
    fn +=(x)                                    # b += x changes b itself
        items.add(x)

a = Vec2(1, 2)
print(a + Vec2(3, 4), -a, a * 3)                # <4, 6> <-1, -2> <3, 6>
print(a == Vec2(1, 2), a != a)                  # true false
print([Vec2(3, 0), Vec2(1, 0)].sort.first)      # <1, 0>
g = Grid()
g[(1, 2)] = 7
print(g[1, 2], g[0, 0])                         # 7 0
b = Bag()
b += 3
print(b.items)                                  # [3]
```

The operators are `+ - * / % ** == < [] []= += -= *= /= %=` and unary `-`. `!=` is derived
from `==`, and `> <= >=` from `<`. The operator methods are public. Without `+=`, `a += b` is
`a = a + b` (a new value); with it, the object changes itself. The type checker knows what
the operator methods return.

## Interfaces

An interface lists methods. Methods without a body are required; methods with a body are
defaults that classes get for free.

```ixo
interface Shape
    fn area
    fn describe = f"a shape of area {area}"

class Square(side) is Shape
    fn area = side * side

class Circle(r) is Shape
    fn area = 3.14159 * r ** 2
    fn describe = "a circle"                    # replaces the default

fn total(shapes: list[Shape]) = shapes.map(_.area).sum

shapes = [Square(2), Circle(1)]
for s in shapes -> print(s.describe)
print(total(shapes))
```

- `class X is A, B` declares that `X` implements `A` and `B`. A missing required method is a
  compile error: `Square doesn't define 'area', which Shape requires`.
- Interfaces are also **structural**: any value whose class has the required methods fits an
  interface type, and `x is Shape` checks exactly that.
- Interface names work as types (`fn f(s: Shape)`), in `is` tests and in patterns
  (`Shape -> ...`). The checker only allows the interface's methods on an interface-typed
  value.
- An interface can **extend** others: `interface Solid is Shape` requires `Shape`'s methods
  too, and a class that is a `Solid` is also a `Shape`.
- The checker compares a class's methods with the interface's: a method with the wrong number
  of parameters, an incompatible return type or the other call form (`fn area` vs
  `fn area()`) is an error.
- Export an interface with `*` like a class: `interface Shape*`.

## Enums

An enum is a closed set of variants. Variants without fields are single values; variants
with fields are constructed like classes. Methods written in the enum are shared by every
variant.

```ixo
enum Color
    Red
    Green
    Rgb(r, g, b)

enum Shape
    Circle(r)
    Rect(w, h)
    Empty
    fn area = match self
        Shape.Circle(r) -> 3 * r * r
        Shape.Rect(w, h) -> w * h
        Shape.Empty -> 0

shapes = [Shape.Circle(2), Shape.Rect(2, 5), Shape.Empty]
print(shapes.map(_.area))                       # [12, 10, 0]
print(Color.Red, Shape.Rect(1, 2))              # Color.Red Shape.Rect(w: 1, h: 2)

fn name(c: Color) = match c
    Color.Red -> "red"
    Color.Green -> "green"
    Color.Rgb(r, g, b) -> f"rgb({r}, {g}, {b})"
print(name(Color.Rgb(1, 2, 3)), Color.Green is Color)
```

- `Color.Red == Color.Red` is true: a variant without fields is one shared value.
- Patterns: `Color.Red` matches that variant, `Shape.Rect(w, h)` destructures, and `Shape`
  alone matches any variant.
- When the checker knows a `match` is over an enum and no arm catches everything, it warns
  about the variants the arms don't handle: `this match doesn't handle Color.Green`.
- Export an enum with `*` (`enum Color*`); its variants and methods come with it.

A small enum fits on one line. In patterns, a variant can be written without its enum's
name when nothing else has that name:

```ixo
enum Shape = Circle(r) | Rect(w, h) | Empty
fn area(s) = match s
    Circle(r) -> 3 * r * r
    Rect(w, h) -> w * h
    Empty -> 0
print([Shape.Circle(1), Shape.Rect(2, 3), Shape.Empty].map(area))   # [3, 6, 0]
```

Methods can follow the one-line form, indented.

## Pattern matching

`match` tries each arm in order. An arm is `pattern [if guard] -> statement`, or a pattern
followed by an indented block.

```ixo
class Point(x, y)

fn describe(v)
    match v
        0 -> "zero"
        1 | 2 | 3 -> "small"                    # alternatives
        4..=9 -> "medium"                       # ranges (numbers or strings)
        int as n if n < 0 -> f"negative {n}"    # type test, binding, guard
        str as s -> f"text of length {s.len}"
        (x, 0) -> f"on the x axis at {x}"       # tuples
        [] -> "empty list"
        [first, ..rest] -> f"{first} and {rest.len} more"   # lists with a rest
        {"type": "circle", "r": r} -> f"circle {r}"  # maps: these keys (others may exist)
        Point(x, y) -> f"point {x}, {y}"        # classes, by constructor fields
        Error.NotFound -> "not found"           # compare against a value
        nil -> "nothing"
        _ -> "something else"                   # anything
```

A map pattern matches a map that has at least the listed keys, with values that match the
patterns after them; `{}` matches any map.

Type names in patterns: `int float num str bool list tuple map set range fn flow task vec
mat quat time rng Error`. A lowercase name binds the value. A capitalized name is a class,
an interface (any value that implements it) or an enum (any of its variants).

## Errors

Ixo separates two kinds of failure:

- **Error values** are for things that can go wrong in a correct program: a missing file, bad
  input, a failed request. Functions return them, and callers handle them.
- **Panics** are bugs: dividing by zero, a missing map key, calling with the wrong number of
  arguments. They stop the program with a message and a stack trace.

```ixo
fn divide(a, b)
    if b == 0 -> return Error.DivByZero("can't divide by zero")
    a / b

r = divide(1, 0)
print(r, r.kind, r.msg, is_error(r))

# handle with a fallback value
v = divide(1, 0) ! err -> 0

# handle with a block; its last expression is the value
w = divide(1, 0) ! err
    print(f"failed: {err.msg}")
    -1

# propagate: `?` returns the error from the current function
fn ratio(a, b)
    q = divide(a, b)?
    q * 100

# retry the failed expression
attempts = 0
fn flaky
    attempts += 1
    if attempts < 3 -> return Error.Busy
    "ok"

x = flaky() ! err -> retry                 # until it succeeds
y = flaky() ! err
    retry 2                                # at most 2 more attempts...
    "gave up"                              # ...then this is the value
z = flaky() ! err -> retry 5, 100ms        # with a delay between attempts
```

`Error.Anything` creates an error of kind `Anything`. `Error.Anything("message")` adds a
message, and `Error.Anything(other_error)` wraps a cause (`err.cause`). `error("msg")` is a
plain `Error`. Errors are false in conditions, `==` compares their kind, and
`err.log_error("prefix")` prints to stderr. A top-level `?` whose error isn't handled ends
the program with the error message.

## Cleanup with defer

`defer` runs a statement or block when the enclosing block ends, however it ends: normally,
with `return`, `break`, `continue`, `retry` or `?`. Several defers run in reverse order.

```ixo
log = []
fn work(fail)
    log.add("open")
    defer log.add("close")
    defer
        log.add("flush")
    if fail -> return "early"
    log.add("work")
    "done"

print(work(false), log)        # done ["open", "work", "flush", "close"]
```

A `defer` inside a loop body runs at the end of each iteration. Defers don't run when the
program panics.

### Files that close themselves

`use f = <value>` binds a value and closes it when the block ends, however it ends:
`f.close` runs as a `defer` would. `fs.open(path, mode)` opens a file to read line by line
(`"r"`, the default), to write (`"w"`) or to append (`"a"`):

```ixo
fn save(path, lines)
    use f = fs.open(path, "w")?
    for l in lines -> f.write(l + "\n")
fn count(path)
    use f = fs.open(path)?
    n = 0
    while f.read_line != nil -> n += 1
    n
save("lines.tmp", ["a", "b"])?
print(count("lines.tmp"))                # 2
fs.drop("lines.tmp")?
```

A `File` has `read_line` (nil at the end), `lines`, `read`, `write(text)` and `close`.

## Types

Types are optional. Annotate parameters, return values, variables and fields where it helps:

```ixo
class Vec2(x: float, y: float)

fn total(items: list[int]): int
    sum = 0
    for i in items -> sum += i
    sum

fn find(names: list[str], target: str): int?     # int or nil
    for i, n in names
        if n == target -> return i
    nil

fn parse(s: str): int ! Error                    # int or an error
    s.int

limit: int = 10
```

Type syntax: `int float num str bool nil any`, `list[T]`, `map[K, V]`, `set[T]`,
`flow[T]`, `(A, B)` for tuples, `fn(A, B) -> R` for functions, class, interface and enum
names, `T?` (or nil), `T ! E` (or an error) and `T!` (short for `T ! Error`).

Single capital letters (`T`, `K`) are type variables, which make functions generic: the
checker binds them from the arguments at each call. Longer names work when declared in
brackets after the name: `fn first[Item](xs: list[Item]): Item`.

```ixo
fn first(xs: list[T]): T = xs[0]
n: int = first([1, 2, 3])        # T is int here
s: str = first(["a", "b"])       # and str here
```

`ixo check` (and `ixo run`, before running) infers types from literals, annotations and the
standard library, and reports definite mistakes:

- calling with the wrong number of arguments
- an argument or return value of the wrong type (`argument 1 of f() should be int, got str`)
- members that don't exist (`str has no method 'upx' (did you mean 'up'?)`)
- impossible operations (`can't use '+' on str and int`)
- assigning the wrong type to an annotated variable or field

The checker **narrows** types where a test makes them certain. After `if x == nil -> return`,
`x` is no longer optional; inside `if s is Circle` (and in a `match` arm `Circle(r) -> ...`),
`s` is a `Circle`, so its own fields are allowed. `and`, `or`, `not` and `while` conditions narrow
too, and assigning to the variable forgets what was learned.

```ixo
class Circle(r)
fn name_len(name: str?): int
    if name == nil -> return 0
    name.len                         # str here, not str?

fn radius(x): float
    if x is Circle -> return x.r
    0.0
print(name_len("ann"), name_len(nil), radius(Circle(2.0)))   # 3 0 2.0
```

Using a member of an annotated optional value without a test is a warning:
`'name' might be nil here; check it first`.

**Generic classes** take type parameters in brackets (`class Cache[Key, Value]`). The checker
infers them from the constructor's arguments or takes them from an annotation:

```ixo
class Box[T](value: T)
    fn get: T = value

class Stack[T]
    items: list[T] = []
    fn push(x: T)
        items.add(x)
    fn pop: T? = if items.len > 0 -> items.take else nil

n: int = Box(41).get + 1
s: Stack[str] = Stack()
s.push("a")
print(n, s.pop)                      # 42 a
```

**Borrows** say what a function may do with a parameter. `p: &T` is read-only: calling a
method that changes it, assigning its fields or passing it on as `&mut` is a compile error.
`p: &mut T` (or `mut p`) allows changes and documents them.

```ixo
class Account(owner, balance)
    fn deposit(n)
        balance += n

fn show(a: &Account) = f"{a.owner}: {a.balance}"     # a.deposit(1) here is an error
fn pay(a: &mut Account, n) = a.deposit(n)
acc = Account("ann", 10)
pay(acc, 5)
print(show(acc))                                      # ann: 15
```

Unannotated parameters are `any`, and `any` fits everywhere, so dynamic code is never rejected.
Annotations are also checked while the program runs: passing a `str` to an `int` parameter
through `any` code panics with a clear message. An `int` is accepted and converted where a
`float` is expected.

### Strict code

`$strict` on a file's first line, or `strict = true` in the project's `ixo.toml`, makes the
checker strict:

- Every parameter has a type: an annotation, its default's type, or what every call in the
  file passes. A function used as a value, or never called, needs annotations; exported
  functions state their parameter and return types.
- Using a value that might be `nil` without a check is an error.
- An error a call can give must be handled (`?`, `!`, `match`), or dropped on purpose with
  `_ = ...`.
- A `match` over an enum handles every variant, or has `_ ->`.
- A `par fn` can't change module variables: each task has its own copy, so the change would
  be lost.
- `ffi` calls (raw memory) go inside an `unsafe` block. `ixo check --unsafe` lists those
  blocks and calls.
- What `fs.open` or `net.listen` gives is bound with `use`, so it's closed.

```ixo-fragment
$strict
fn find(names: list[str], target: str): int?
    for i, name in names
        if name == target -> return i
    nil
at = find(["a"], "b")
print(at + 1)         # error: a value that might be nil is used with '+'
```

Code without `$strict` is gradual, as before: these are warnings, or allowed. A strict
file can use gradual modules, whose values are checked as they cross. `ixo fmt --annotate`
writes into a file's signatures the types the checker infers: parameters from their calls,
results from the bodies.

**Integer overflow.** Ints are 64-bit and wrap around, as in C or in Rust's release builds.
`ixo test`, and `IXO_CHECK_OVERFLOW=1` for any run, stop at an overflow instead, naming the
operation.

## Modules

A file is a module. Only names marked with `*` right after the name are exported:
functions, classes, interfaces, enums and module variables.

```ixo-fragment
# geometry.ixo
fn area*(w, h) = w * h       # exported
fn helper(x) = x             # private
class Rect*(w, h)
unit* = "cm"                 # an exported variable: importers can read it, not change it
```

```ixo-fragment
use geometry                 # everything exported, plus `geometry` itself
print(area(2, 3), geometry.area(2, 3))

in geometry use area, Rect   # only these names
in geometry use area as surface

use shapes.circles           # the file shapes/circles.ixo
use math                     # the built-in modules: math fs os net json
```

Modules are looked up next to the importing file, then in the **project root** (the nearest
directory above it with an `ixo.toml`), then in installed packages (`ixo_modules/`), then in
the directories listed in the `IXO_PATH` environment variable. So a test in `tests/` can
`use lib.greeting` from the project root. A module's top-level code runs once, the first
time it's imported. Circular imports are an error.

`ixo new name` creates a project:

```text
name/
  ixo.toml                 [package] name, version, main; [dependencies]
  main.ixo                 the program (ixo main.ixo)
  lib/greeting.ixo         a module (use lib.greeting)
  tests/greeting_test.ixo  @test functions (ixo test)
  .vscode/launch.json      F5 debugs main.ixo
```

A program can find its own file with `os.script` and its folder with `os.script_dir`, for
data files that live next to it.

## Packages and building

A package is a folder or git repository with an `ixo.toml`. Add one to a project and `use` it
by name:

```text
ixo init                                    # make the current folder a project (ixo.toml)
ixo add https://github.com/someone/ixo-json5.git
ixo add https://github.com/someone/geo.git --rev v1.2   # a tag, branch or commit
ixo add ../shared                           # a local folder
ixo install                                 # after a fresh clone: get what ixo.lock says
ixo update [name]                           # move to the newest commits
ixo remove geo
```

```toml
[dependencies]
json5 = { git = "https://github.com/someone/ixo-json5.git" }
geo = { git = "https://github.com/someone/geo.git", rev = "v1.2" }
shared = { path = "../shared" }
```

- Packages are installed in `ixo_modules/<name>/`, together with the packages they need
  (one copy of each, shared). `use json5` loads the package's main file (`main` in its
  `ixo.toml`, else `lib.ixo` or `json5.ixo`); `use geo.shapes` loads a module inside it.
- `ixo.lock` records the exact commit of every package, so commit it: `ixo install` then gives
  everyone the same code. `ixo update` is the only command that moves to newer commits.
- When two packages want different versions of a third, the project's own choice wins, then
  the first one found; a note says so. Two different packages with the same name are an
  error.
- Packages come from git, so `git` must be installed.

`ixo build` packs a program, its modules and its packages into one executable that runs
without Ixo installed:

```text
ixo build                     # the project's main file -> ./main (main.exe on Windows)
ixo build tool.ixo -o dist/tool
./dist/tool input.txt         # every argument goes to the program
```

The program is checked first, and errors stop the build. Only modules are packed; data files
the program reads stay outside.

**Other machines.** The packed program is the same everywhere; `--target` puts it on the
runtime of another system: `windows-x64`, `windows-arm64`, `linux-x64`, `linux-arm64`,
`macos-x64`, `macos-arm64` or `wasm`.

```text
ixo build tool.ixo --target linux-arm64                          # the runtime from the cache
ixo build tool.ixo --target linux-arm64 --runtime path/to/ixo    # ... or given here
ixo build tool.ixo --target wasm                 # a folder with a web page that runs it
ixo build tool.ixo --train -- sample.txt         # one run first: the executable starts warm
```

- The runtime for each target is kept in `~/.ixo/runtimes/<version>/` (`IXO_HOME` moves
  it). When it isn't there, it is fetched once from the release.
- `--runtime` gives one directly: an `ixo` executable, or for `wasm` the folder with
  `ixo.js` and `ixo.wasm`.
- `--train` runs the program once and packs which functions got hot, so the built
  executable compiles them early.

## Concurrency

Tasks are lightweight: thousands are fine. `fork` tasks take turns on one OS thread, so
they give concurrency; for parallelism, see [`par`](#parallelism). Tasks communicate through
**flows** (channels).

```ixo
jobs = flow[int](10)          # buffered (up to 10 waiting values)
results = flow[int](10)
done = flow()                 # unbuffered: a send waits for a receiver

fork fn worker(input, output)   # `fork fn`: every call starts a new task
    for n <: input              # receive until the flow is closed and empty
        n * n :> output         # send

for i in 1..=3 -> worker(jobs, results)
for n in 1..=5 -> n :> jobs
jobs.close

total = 0
for _ in 1..=5
    r <: results                # receive one value
    total += r
print(total)                    # 55
```

- `fork f(x)` runs any call as a task and returns a task value. `t.await` waits for its
  result, and `t.done` reports whether it has finished.
- `fork compute(x) -> r` followed by an indented block runs the block in the task with the
  result bound to `r`.
- `with tasks` + block: waits for every task forked inside the block.
- `for a <: flow_a, b <: flow_b` receives from several flows in step.
- `500ms.wait` (or `2s.wait`) pauses the current task.
- Long loops give other tasks a turn automatically.
- `select` waits for whichever flow is ready first:

```ixo
fast = flow()
slow = flow()
fork fn send_later(out, value, delay)
    delay.wait
    value :> out
send_later(fast, "fast", 5)
send_later(slow, "slow", 50)
select
    x <: fast -> print("got", x)           # got fast
    y <: slow -> print("got", y)
select
    y <: slow -> print("got", y)           # got slow
    after 1000 -> print("timed out")       # after ms: a timeout
```

  An `else` arm runs at once when no flow has a value ready. A closed flow gives `nil`.
  `select` works with flows shared with `par` workers too.
- The program ends when the main code is done and no task can make progress. If the main
  code waits for something that can never happen, it stops with a "deadlock" panic.

## Parallelism

`par` runs a call on another CPU core and returns a task, just like `fork`:

```ixo
fn count_primes(lo, hi)
    n = 0
    for x in lo..hi
        if x < 2 -> continue
        prime = true
        d = 2
        while d * d <= x
            if x % d == 0
                prime = false
                break
            d += 1
        if prime -> n += 1
    n

t = par count_primes(0, 10000)
print(t.await)                                   # 1229

chunks = (0..8).map(par count_primes(_ * 1250, (_ + 1) * 1250))
print(chunks.map(_.await).sum)                   # 1229, computed on up to 8 cores

par fn square(x) = x * x                         # every call is a par task
print(square(7).await, cores())
```

A par task runs in its own heap, so there are no data races:

- The arguments (and the function, if it's a closure) are **copied** into the task. Lists,
  maps, instances, closures and nested structures all copy; flows don't (see below). The
  profilers (`--profile`, `--memprofile`) report how much par copied.
- Calling a `par fn` (or `fork fn`) gives a task, not a value: using it as a value is an
  error that suggests `.await`.
- The task sees a **snapshot** of the module variables it uses, taken when it starts.
  Changes it makes to them stay in the task.
- The result is copied back to the caller when you `await` it. Errors come back as errors;
  a panic in a par task stops the program.
- **Flows are shared.** Passing a flow to par tasks is how they stream results or split work:

```ixo
jobs = flow[int](100)
out = flow[int](100)
par fn worker(jobs, out)
    for j <: jobs -> j * 2 :> out

with tasks
    for _ in 0..4 -> worker(jobs, out)
    for j in 1..=20 -> j :> jobs
    jobs.close
out.close
total = 0
for v <: out -> total += v
print(total)                                     # 420
```

`with tasks` waits for par tasks too. Use `par` for CPU-heavy work; the copying makes it
costly for tiny calls. `cores()` tells how many run at once.

Large read-only data doesn't need copying: `freeze(x)` makes a value deeply immutable, and par
tasks then share it instead of copying it. Changing a frozen value panics; `frozen(x)` tells
whether a value is frozen, and methods like `map` return ordinary (changeable) values.

```ixo
table = freeze((0..100000).map(_ * 2))
par fn lookup(t, i) = t[i]
print((0..4).map(par lookup(table, _ * 1000)).map(_.await))   # [0, 2000, 4000, 6000]
```

A panic in a par task stops the program and names the task's function. The debugger and the
profilers see par workers too.

## GPU compute

A `gpu fn` is a kernel: a function that runs once per thread, many threads at a time, on the
GPU. Its threads share buffers of numbers; each thread finds its position with `gpu.index`.

```ixo
gpu fn saxpy(a: float, x: buffer[float], y: buffer[float])
    i = gpu.index
    if i < y.len -> y[i] = a * x[i] + y[i]

x = gpu.floats([1.0, 2.0, 3.0])        # a buffer from a list
y = gpu.floats(3)                      # three zeros
saxpy(2.0, x, y).await                 # one thread per item of the first buffer
print(y.list)                          # [2.0, 4.0, 6.0]
```

- **Buffers:** `gpu.floats(n or list)`, `gpu.ints(n or list)`, `gpu.buffer(list)` (floats if any
  item is a float). On the device, numbers are 32-bit, as in WGSL and on most GPUs: an int
  that doesn't fit is an error, and floats keep about 7 digits. A buffer has `.len`, `.list`,
  `.upload(xs)`, `b[i]` and `b[i] = v`, and `.free` (the collector frees it otherwise). The
  CPU reads and writes buffers in place: no copies.
- **Launching:** calling a `gpu fn` starts one thread per item of its first buffer argument
  and gives a task, already finished: launches run in order. `gpu.launch(n, kernel, args...)`
  starts `n` threads instead.
- **What a gpu fn can use:** parameters typed `int`, `float`, `bool`, `buffer[int]` or
  `buffer[float]`; its own variables (ints, floats and bools); arithmetic (`//` and `%` floor,
  as everywhere in Ixo; dividing an int by 0 gives 0 on the device); comparisons, `and`, `or`,
  `not`, bit operators; `if`, `while`, `for i in a..b (by a constant)`, `break`, `continue`,
  `return`; math (`sqrt`, `abs`, `min`, `max`, `floor`, `ceil`, `sin`, `cos`, `tan`, `exp`, `log`,
  `pow`, `float`, `int`, and their method forms like `x.sqrt`). Anything else (strings, lists,
  maps, closures, other calls) is a compile error that says what isn't allowed.
- **Indexing outside a buffer** stops the program with the kernel, the index and the buffer.
- **Where it runs:** on a GPU through Vulkan (Windows, Linux; macOS through MoltenVK), loaded
  when the program first uses `gpu`. Without a GPU or Vulkan, the same kernels run on all
  CPU cores, with the same results (`IXO_GPU=cpu` forces that). `gpu.device` names the GPU
  in use (or `"cpu"`), `gpu.devices` lists them, `IXO_GPU_DEVICE=n` picks one.
- **For the web:** `gpu.wgsl(kernel)` gives the kernel as WGSL for WebGPU (and `gpu.spirv`
  as SPIR-V words), with the same bindings: the buffers in order, a status buffer, then the
  scalars in a uniform.

`bench/gpu` compares kernels on the GPU, on the CPU fallback, as plain Ixo loops and as C++.

Threads past the launch's count don't run, so a kernel needs no check for that (the
examples above keep one only to show indexing).

**Maps on the GPU, and on every core.** `gpu` before a `map` runs its lambda as a kernel:

```ixo
xs = (0..8).map(_ * 1.5)
k = 2.0
print(gpu xs.map(fn(v) = v * v + k))     # [2.0, 4.25, 11.0, 22.25, 38.0, 58.25, 83.0, 112.25]
```

- The lambda is one expression over numbers; the variables it uses from outside come in as
  numbers.
- Without `gpu`, such a `map` over a big list of numbers (200,000 items or more) is spread
  over all cores by itself.
- It runs with Ixo's own 64-bit arithmetic, so the result is exactly the plain loop's;
  `IXO_AUTO_PAR=0` turns that off, and `IXO_AUTO_PAR=n` sets the size.

**Groups of threads.** Threads run in groups of 64, which can share memory and wait for
each other:

- `gpu.local` is a thread's place in its group (0 to 63), `gpu.group` the group's number;
- `name = gpu.shared_floats(64)` (or `gpu.shared_ints`) makes an array each group shares;
- `gpu.barrier()` waits until every thread of the group has got there.

A kernel that uses them runs whole groups, so threads past the first buffer's length run
too: it checks its own indexes.

```ixo
gpu fn block_sums(xs: buffer[float], sums: buffer[float])
    tile = gpu.shared_floats(64)
    i = gpu.index
    t = gpu.local
    v = 0.0
    if i < xs.len -> v = xs[i]
    tile[t] = v
    gpu.barrier()
    step = 32
    while step > 0
        if t < step -> tile[t] = tile[t] + tile[t + step]
        gpu.barrier()
        step = step // 2
    if t == 0 -> sums[gpu.group] = tile[0]

sums = gpu.floats(4)
block_sums(gpu.floats((1..=200).map(_.float)), sums)
print(sums.list)                         # [2080.0, 6176.0, 10272.0, 1572.0]
```

**2D and 3D launches.** `gpu.launch((w, h), kernel, args...)`, or `(w, h, d)`, runs w × h
threads; `gpu.x`, `gpu.y` and `gpu.z` give each thread's place.

**vec and mat in kernels.** Local variables can hold `vec(a, b)` up to `vec(a, b, c, d)`, and
parameters can be `vec2`, `vec3`, `vec4` or `mat` (4x4). They have `+`, `-`, `*` (and `/` by
a number), `.x .y .z .w`, `.len`, `.norm`, `.dot(v)`, `m * v` and `m[i, j]`.

**Unchecked and foreign kernels.**

- `unsafe gpu fn` skips the index checks on the device (the CPU fallback keeps them).
- `buffer.address` is where a buffer's items are in memory, for `ffi` calls that take a
  pointer.
- `gpu.ptx(text, name)` loads a kernel written for NVIDIA GPUs (PTX, as nvcc or nvrtc make
  it) through the CUDA driver. `k.launch(n, [args...])` runs n threads, copying buffers in
  and back; without an NVIDIA driver, `gpu.ptx` gives an error.

The CPU fallback compiles kernels to machine code on x86-64 and ARM64 (`IXO_GPU_JIT=0` runs
its register code instead). In the browser playground, kernels run through WebGPU.

## Memory regions

A region block marks memory that is only needed inside it. When the block ends, whatever it
allocated that is no longer reachable is freed at once, without waiting for the garbage
collector. Values that are still reachable, because they were returned, stored elsewhere or
kept in a variable outside the block, survive. Regions never make a program incorrect; they
only make memory use more predictable.

```ixo
kept = []
for frame in 0..60
    @arena
        scratch = []                      # a variable of the block
        for i in 0..1000 -> scratch.add([i, i * 2])
        kept.add(scratch.len)             # an int escapes: fine

fn build(n)
    @arena
        parts = (0..n).map(str(_))
        return parts.join(",")            # the string survives, `parts` is freed
print(build(5), kept.len)
```

- `@arena` + block: variables assigned inside the block belong to it. A comment can say
  what the block is for (a frame, a request).
- `@cache name` + block: `name` is a map that keeps its contents across calls (one per
  place in the code), for memoizing.
- `@dual name = initial` + block: a double buffer. `name.current` and `name.other` are two
  values, `name.swap` exchanges them, and the pair persists across calls.

```ixo
fn level(n)
    @cache levels
        if not levels.has(n) -> levels.set(n, f"level {n} data")
        levels.get(n)
print(level(1), level(1))

fn render
    @dual buffers = []
        buffers.current.add("frame")
        buffers.swap
        [buffers.current.len, buffers.other.len]
print(render(), render())                        # [0, 1] [1, 1]
```

`memory()` reports the bytes the program's objects use, and `gc()` runs the collector.

## More of the standard library

The [reference](stdlib.md) lists everything; here are the larger modules.

**Regular expressions** (`re`) use ECMAScript syntax. Raw strings save escaping:

```ixo
print(re.test(r"\d+", "abc 42"))                        # true
m = re.find(r"(\w+)@(\w+)", "mail ann@home now")
print(m["text"], m["groups"])                            # ann@home ["ann", "home"]
print(re.find_all(r"\d+", "1 22 333"))                  # ["1", "22", "333"]
print(re.replace(r"(\d+)", "a1b22", "<$1>"))            # a<1>b<22>
print(re.replace(r"\d+", "a1b22", fn(s) = str(s.int * 2)))   # a2b44
print(re.split(r",\s*", "a, b,c"))                      # ["a", "b", "c"]
```

**Dates and times** (`time`) are local by default; `time.utc(...)` makes UTC times:

```ixo
t = time(2024, 2, 29, 13, 5, 9)
print(t.show("YYYY-MM-DD HH:mm"), t.day_of_year)        # 2024-02-29 13:05 60
print(t.add_days(1).date, time.utc(2024, 1, 1).iso(true))   # 2024-03-01 2024-01-01T00:00:00Z
p = time.parse("01/02/2023 07:30", "DD/MM/YYYY HH:mm")
print(p.month, p.day, time(2024, 1, 2).diff(time(2024, 1, 1)))   # 2 1 86400.0
```

**CSV** (`csv.parse`, `csv.format`, `csv.read`, `csv.write`) handles quoting. With a header
row, each row is a map:

```ixo
rows = csv.parse("name,age\nAnn,31\n\"Smith, Bo\",40\n")
print(rows[1]["name"], rows[0]["age"])                  # Smith, Bo 31
print(csv.format([["x", "y,z"]]).trim)                  # x,"y,z"
```

**Programs, C libraries and HTTP** (not in the browser playground):

```ixo-fragment
r = os.exec(["git", "status", "--short"])               # {code, out, err, ok}
if r["ok"] -> print(r["out"])

libc = ffi.load("libc.so.6")                            # msvcrt.dll on Windows
strlen = ffi.func(libc, "strlen", "u64(str)")
print(strlen("hello"))                                  # 5

page = net.read("https://example.com")                  # GET; errors are Error values
res = net.request("POST", "https://example.com/api", {"n": 1})   # maps are sent as JSON
print(res["status"], res["body"])

server = net.listen(8080)                               # a flow of requests
for req <: server
    print(req.method, req.path, req.query)
    if req.path == "/hello" -> req.respond(200, "hi")
    else req.respond(404, "not found")
```

HTTPS works without other programs on Windows; on Linux and macOS it uses `curl`. Each request
to `net.listen`'s server arrives as a `Request` with `method`, `path`, `query`, `headers`,
`body` and `json`; answer it with `req.respond(status, body, headers)` (a map or list body is
sent as JSON).

## Performance

Ixo compiles to bytecode for a stack VM with fixed field slots and cached method lookups. On
x86-64 and ARM64, a **JIT** compiles hot functions and loops to machine code: a function after
about 200 calls, a loop after about 2000 iterations, without anything to write. `$jit` before
a function (or a `for` / `while` loop) compiles it at once, and `$nojit` keeps a function in
the interpreter:

```ixo
$jit
fn count_multiples(n, k)
    c = 0
    for i in 0..n
        if i % k == 0 -> c += 1
    c
print(count_multiples(1000000, 7))      # 142858
```

The machine code handles these itself:
- int and float arithmetic and comparisons
- locals, module variables and variables captured from an enclosing function
- loops, list indexing and fields
- method calls, constructors and calls to other compiled functions
- calls to built-in functions and methods (`x.sqrt`, `xs.add(v)`, `s.cut(",")`)

Most other instructions (string building, map access, literals) are run for it by the
interpreter, one instruction at a time, without leaving machine code. Only a call to a
function that can't be compiled makes the interpreter take over, until the next hot loop.
Results never differ, and long loops still let other tasks run. `IXO_JIT=0` turns the JIT
off; `IXO_JIT_LOG=1` lists what it compiles.

Some patterns are fast in the interpreter too:
- `for i in a..b` counts without making a range object.
- `s += x` (or `s = s + x`) grows the string in place when nothing else has seen it: a
  local only grown in that loop, or a module variable nobody has read since the last
  append. Building a string piece by piece takes linear time, not quadratic.
- An f-string is built in one buffer.
- Small maps (up to 8 keys) don't hash at all.

Sorting with a comparator of the form `_a < _b`, `_b > _a` or `_a.age < _b.age` compares
directly, without calling the lambda for each comparison.

Memory:
- Every value (a number, a reference to a string or list...) takes 16 bytes in a list,
  map, field or variable.
- Objects have a 12-byte header: a short string takes 48 bytes and an instance 32 bytes
  plus 16 per field.
- Lists, maps and sets made from literals or JSON are exactly their size, and lists give
  memory back when they shrink a lot.
- The garbage collector runs first at 4 MB, then whenever the heap has doubled. Above
  32 MB of live data it runs when the heap has grown by half.
- `memory()` reports the bytes in use, and `--memprofile` shows where they come from.

`bench/langs/run.py` runs the same programs in Ixo, C++ and Python. On the reference
machine (x86-64, Windows), Ixo takes about 3× the time of C++ on most of them and runs
1.1–15× faster than Python (2.8× faster in total).

To see where time goes, run with `--profile`. It is a sampling profiler that charges time
to functions and lines, including code running as machine code. `--profile-out=f.txt`
also writes folded stacks for flame graph tools. `--memprofile` reports what allocates
memory, how much is live and how long garbage collection took; it runs without the JIT, so
allocation sites are exact. Both include par workers. `IXO_JIT_STATS=1` lists where machine
code hands over to the interpreter (function, line and instruction) and which functions or
loops the JIT gave up on, with the number of compiles and their total time; `IXO_JIT_LOG=1`
prints each compile as it happens. `IXO_GC_STATS=1` prints how long each garbage collection
spends marking and sweeping.

Lists that hold only ints, only floats or only bools are stored packed (8 bytes per int or
float, 1 per bool, instead of 16); this never shows in what a program does. `IXO_JIT_CACHE=1`
remembers which functions got hot, so the next run compiles them sooner (`ixo cache clean`
forgets it); it is off by default because today it measures no gain.

More that happens by itself:

- **Compiling beside the program.** Big functions compile on a thread of their own while the
  program runs on, and the new code is installed at the next call or loop entry, so
  compiling doesn't pause the program; small ones compile at once, in microseconds.
  `IXO_JIT_SYNC=1` compiles everything on the program's thread.
- **Code kept up to date.** Code that is replaced (compiled again, with or without its type
  guesses) is freed once nothing runs it. Code made before types were known is made again
  with them once the function has run a while.
- **Small functions in place.** A call of a small function of the same module is compiled
  where it is, without a call. Small means one expression or a few statements, and not
  recursive there. A small function passed by name to `map`, `keep`, `fold`... runs inside
  the loop like a lambda. `IXO_INLINE=0` turns that off; debugging and profiling keep every
  call.
- **Fewer checks and objects.** In typed machine code, a variable checked once to hold a
  list isn't checked again until it changes. `a, b = x, y` makes no tuple, and a list of
  constants that a `for` loop goes over is made once.
- **Sums in one call.** `for x in xs -> s += x` over numbers adds them all in one call (SIMD
  for ints), with the loop's result.
- **Maps on every core**, as described under GPU compute.

## Tools

| command | what it does |
|---|---|
| `ixo file.ixo [args]` | run a program |
| `ixo run --watch file.ixo` | run, and run again whenever the file or a module it uses changes |
| `ixo` or `ixo repl` | interactive REPL (see below) |
| `ixo new name` | create a project |
| `ixo init`, `ixo add`, `ixo install`, `ixo update`, `ixo remove` | packages (see [Packages](#packages-and-building)) |
| `ixo build [file] [-o out]` | pack a program into one executable |
| `ixo cache clean` | forget the hot functions remembered by `IXO_JIT_CACHE=1` runs |
| `ixo check files...` | syntax, name and type errors, without running |
| `ixo fmt [--check] files or dirs` | format: 4-space indentation, spacing; comments kept |
| `ixo fmt --upgrade files or dirs` | rewrite old spellings (see [Upgrading from 0.5](#upgrading-from-05)), then format |
| `ixo test [files or dirs]` | run the `@test` functions (default: `tests/`) |
| `ixo test --filter text` | only tests whose name contains `text` (`--watch`, `--json` too) |
| `ixo --profile file.ixo` | run with the sampling profiler (report on stderr) |
| `ixo --profile-out=f.txt file.ixo` | also write folded stacks for flame graph tools |
| `ixo --memprofile file.ixo` | report allocations, live memory and collections |
| `ixo lsp` | language server for editors |
| `ixo dap` | debug adapter for editors (breakpoints, stepping, variables) |
| `ixo disasm file.ixo` | show the bytecode |
| `ixo stdlib-docs` | print the standard library reference |

`--no-typecheck` runs a program without the static checker.

A test function is any function marked `@test`. It passes unless it panics or returns an error:

```ixo
@test
fn addition_works
    assert_eq(1 + 1, 2)
    assert(3 > 2, "three is bigger")
```

When `assert_eq` fails on collections, strings or objects, it says where they first differ:

```text
  FAIL tests/shop_test.ixo: totals
       assertion failed: [1, 2, [3, 4]] != [1, 2, [3, 5]]
         first difference at [2][1]: 4 vs 5
```

**The REPL** keeps what you define. Arrow keys edit the line and browse the history (saved
in `~/.ixo_history`), Tab completes names and members (`items.` Tab lists the methods of the
value in `items`), and Ctrl+C clears the input. Commands: `:type expr` shows a static type,
`:load file` runs a file in the session, `:reload` runs it again after you edit it, `:quit`
leaves.

**Editors.** `ixo lsp` gives errors as you type, hover (with docstrings), go to definition,
find references and rename across the workspace, signature help, completion (with snippets,
and every variant after `match` on an enum), inlay hints (inferred types and parameter
names), semantic highlighting, and quick fixes: add the methods an interface requires, add
missing `match` arms, apply "did you mean", import an unknown name from the module that
exports it, export a private function, and update old spellings (one at a time or the whole
file).

**Debugging.** `ixo dap` runs a program under the debugger: breakpoints (with conditions
like `x > 10`, hit counts like `>= 3`, and log messages like `total is {total}`), stepping,
the call stack, variables you can expand and change, and a debug console that evaluates any
expression in the selected frame. It stops when the program panics. Each `par` worker
appears as its own thread, with its own call stack and variables. In VS Code, press F5 in
an `.ixo` file, or use the "Debug test" link above a `@test` function.

## Upgrading from 0.5

Ixo 0.6 changed some syntax so there's one short way to write each thing. Old programs still
run: every old spelling gives a `deprecated:` warning with its fix, and

```text
ixo fmt --upgrade src tests        # rewrites the files, keeping what they do
```

applies all of them (editors offer the same fixes one by one). Version 0.7 drops the old forms.

| before (0.5) | now |
|---|---|
| `fn f(x) -> int` | `fn f(x): int` |
| `fn(x) -> x * 2` | `fn(x) = x * 2` |
| `fn calc(x)*` | `fn calc*(x)` |
| `if a -> 1 else -> 2` | `if a -> 1 else 2` |
| `0..<10`, `for i in 0..10, 2`, `xs[.., -1]` | `0..10`, `for i in 0..10 by 2`, `xs[.. by -1]` |
| `7 ~/ 2` | `7 // 2` |
| `a // 3` (cube root) | `a.root(3)` |
| `null`, `&&`, `\|\|`, `!x`, `===` | `nil`, `and`, `or`, `not x`, `is` |
| `var x: int = 1` | `x: int = 1` |
| `Result[int, Error]` | `int ! Error` (or `int!`) |
| `sleep(500)` | `500.wait` |
| `$hot` | `$jit` |
| `@temp`, `@frame`, `@req`, `@thread` | `@arena` |
| `for i, x in xs.indexed` | `for i, x in xs` |
| `\{` in strings | `{` (`{{` in f-strings) |

Four changes keep the same text but change what it means; the upgrade rewrites the first
three so programs behave as before:

- `-2 ** 2` is `-4` and `-5.abs` is `-5` (they were `4` and `5`): the upgrade writes
  `(-2) ** 2` and `(-5).abs`.
- `for a, b in list` gives the position and the item; to unpack pairs, write
  `for (a, b) in list`. The upgrade adds the parentheses where the checker knows the items are
  pairs, and a loop that meets pairs anyway says so once while the program runs.
- `//` is integer division (it was the n-th root): use `x.root(n)` for roots.
- A function declared without parentheses (`fn next`) is called wherever its name is used. To
  pass or return it as a value, declare it with parentheses (`fn next()`) or write `&next`.

## Upgrading to 1.0

Ixo 1.0 adds strict code, `data` records, numeric arrays and the rest of this guide's newer
parts without taking anything away. A few meanings changed; the old ones were errors or
rare:

- `a < b < c` compares in a chain (`a < b and b < c`); before, it compared a bool with `c`.
- `fn main` with parameters takes them from the command line, and arguments it doesn't
  expect are an error. A program that calls `main(...)` itself is unchanged.
- `m[k] += v` on a map with a missing key starts from 0 (or `""`, `[]`) instead of
  panicking.
- Floats print as Python prints them: `990000000.0`, not `9.9e+08`. Scientific notation is
  used from `1e16` up and below `1e-4`.
- `ixo test` stops at an integer overflow.

`ixo fmt --upgrade` still rewrites the 0.5 spellings above.
