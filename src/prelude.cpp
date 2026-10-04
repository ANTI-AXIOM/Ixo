// The prelude: standard library functions written in Ixo itself.
//
// A function named `type__method` whose first parameter is `self` becomes a
// method on that type (`iter` means every iterable type). Writing these in
// Ixo (instead of C++) lets their callbacks do anything, including waiting on
// flows and tasks.
namespace ixo {

extern const char* const kPreludeSource;
const char* const kPreludeSource = R"IXO(
fn iter__each(self: iter, f: fn(any))
    "Calls f with every item; returns the collection."
    for x in self
        f(x)
    self

fn iter__map(self: iter, f: fn(any) -> any): list
    "A list of f(item) for every item."
    out = []
    for x in self
        out.add(f(x))
    out

fn iter__keep(self: iter, f: fn(any) -> bool): list
    "The items for which f returns true."
    out = []
    for x in self
        if f(x) -> out.add(x)
    out

fn iter__skip(self: iter, f: fn(any) -> bool): list
    "The items for which f returns false."
    out = []
    for x in self
        if not f(x) -> out.add(x)
    out

fn iter__fold(self: iter, start, f: fn(any, any) -> any)
    "Combines the items: fold(0, _a + _b) sums them."
    acc = start
    for x in self
        acc = f(acc, x)
    acc

fn iter__any(self: iter, f: fn(any) -> bool): bool
    "True if f is true for at least one item."
    for x in self
        if f(x) -> return true
    false

fn iter__all(self: iter, f: fn(any) -> bool): bool
    "True if f is true for every item."
    for x in self
        if not f(x) -> return false
    true

fn iter__count(self: iter, f): int
    "How many items match: count(_ > 2) or count(value)."
    n = 0
    is_fn = type(f) == "fn"
    for x in self
        if is_fn
            if f(x) -> n += 1
        elif x == f
            n += 1
    n

fn iter__find_by(self: iter, f: fn(any) -> bool)
    "The first item for which f is true, or nil."
    for x in self
        if f(x) -> return x
    nil

fn iter__min_by(self: iter, f: fn(any) -> any)
    "The item with the smallest f(item)."
    best = nil
    best_key = nil
    for x in self
        k = f(x)
        if best_key == nil or k < best_key
            best = x
            best_key = k
    best

fn iter__max_by(self: iter, f: fn(any) -> any)
    "The item with the largest f(item)."
    best = nil
    best_key = nil
    for x in self
        k = f(x)
        if best_key == nil or k > best_key
            best = x
            best_key = k
    best

fn iter__sum_by(self: iter, f: fn(any) -> num): num
    "The sum of f(item)."
    total = 0
    for x in self
        total += f(x)
    total

fn iter__group_by(self: iter, f: fn(any) -> any): map
    "A map from f(item) to the list of items with that key."
    groups = {}
    for x in self
        k = f(x)
        if k in groups
            groups[k].add(x)
        else
            groups[k] = [x]
    groups

fn iter__partition(self: iter, f: fn(any) -> bool)
    "A tuple (matching, not_matching)."
    yes = []
    no = []
    for x in self
        if f(x)
            yes.add(x)
        else
            no.add(x)
    (yes, no)

fn iter__flat_map(self: iter, f: fn(any) -> any): list
    "Maps each item to a list and joins the lists."
    out = []
    for x in self
        for y in f(x)
            out.add(y)
    out

fn map__each(self: map, f: fn(any, any))
    "Calls f(key, value) for every entry; returns the map."
    for k, v in self
        f(k, v)
    self

fn map__keep(self: map, f: fn(any, any) -> bool): map
    "The entries for which f(key, value) is true, as a new map."
    out = {}
    for k, v in self
        if f(k, v) -> out[k] = v
    out

fn map__map(self: map, f: fn(any, any) -> any): list
    "A list of f(key, value) for every entry."
    out = []
    for k, v in self
        out.add(f(k, v))
    out

class Cache
    "A value that survives between runs of an `@cache name` block."
    data = {}
    fn get*(key, default = nil) = self.data.get(key, default)
    fn set*(key, value)
        self.data[key] = value
        value
    fn has*(key) = key in self.data
    fn load*(path)
        "Reads a file once and keeps its contents."
        if path in self.data -> return self.data[path]
        text = fs.read(path)?
        self.data[path] = text
        text
    fn drop*(key) = self.data.drop(key)
    fn keys* = self.data.keys
    fn size* = self.data.len
    fn is_empty* = self.data.is_empty
    fn clear* = self.data.wipe
    fn reset* = self.data.wipe

class Request(method, path, query, headers, body, id)
    "An HTTP request from net.listen. Answer it with respond."
    fn respond*(status = 200, body = "", headers = nil) = net.reply(self, status, body, headers)
    fn json* = json.parse(self.body)

class Dual(current = nil, other = nil)
    "Two buffers for `@dual name`: work on current, then swap."
    fn swap*
        old = self.current
        self.current = self.other
        self.other = old
        self

fn int__times(self: int, f: fn(int))
    "Calls f(i) for i from 0 to n - 1."
    for i in 0..self
        f(i)
    nil
)IXO";

}  // namespace ixo
