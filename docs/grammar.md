# Ixo grammar

EBNF-style summary of what `src/parser.cpp` accepts. `NEWLINE`, `INDENT` and `DEDENT` come
from the lexer: a line indented deeper than the previous one starts a block (`INDENT`), a
shallower line ends blocks (`DEDENT`). Inside `( )`, `[ ]` and `{ }` newlines and indentation
are ignored, and a line ending in `\` continues on the next line, with one exception: a line
inside brackets that ends with a block header (`fn(x)`, `fn`, `else`, `match x`, an `if`
without `->`) followed by a deeper-indented line opens an indented block there, which ends at
a line indented no deeper than the header or at the closing bracket.

The spellings Ixo 0.5 used (`->` for return types, `..<`, `~/`, `var`, `null`, `&&`, `||`,
`!x`, `===`, `Result[T, E]`, the export `*` after the parameters, a step after a comma,
`@temp` and its siblings, `$hot`) are still accepted, with a warning; they aren't listed here.

```ebnf
program     = { statement NEWLINE } ;

block       = "->" statement                        (* one statement on the same line *)
            | NEWLINE INDENT { statement NEWLINE } DEDENT ;

statement   = fn_decl | class_decl | interface_decl | enum_decl | use_stmt | "while" expr block
            | for_stmt | "return" [ expr ] | "break" | "continue"
            | "retry" [ expr [ "," expr ] ]         (* count and/or delay, inside ! handlers *)
            | "with" "tasks" block
            | select_stmt
            | "defer" block_or_stmt                 (* runs when the enclosing block ends *)
            | "@" region [ NAME [ "=" expr ] ] { "$" NAME } block  (* @arena, @cache c, @dual b = [] *)
            | { "@" NAME | "$" NAME } ( fn_decl | statement )   (* @test, $jit, $nojit *)
            | NAME "*" [ ":" type ] "=" expr        (* an exported module variable *)
            | "todo" [ STRING ]
            | simple ;

simple      = expr
            | target "=" expr
            | NAME ":" type "=" expr
            | target ( "+=" | "-=" | "*=" | "/=" | "%=" | "**=" ) expr
            | target ( "++" | "--" )
            | NAME "<:" expr                        (* receive from a flow *)
            | target { "," target } "=" expr { "," expr } ;
target      = NAME | postfix "." NAME | postfix "[" index "]" ;

block_or_stmt = block | statement ;
region      = "arena" | "cache" | "dual" ;

fn_decl     = [ "fork" | "par" ] "fn" fn_name [ "*" ] [ type_params ] [ "(" params ")" ]
              [ ":" type ] [ "!" [ type ] ]
              ( "=" expr | block | (* nothing: empty body *) ) ;
                    (* without "(...)" the name alone calls it: fn tick, fn alive = hp > 0 *)
fn_name     = NAME
            | "+" | "-" | "*" | "/" | "%" | "**" | "==" | "<" | "[" "]" | "[" "]" "="
            | "+=" | "-=" | "*=" | "/=" | "%=" ;     (* operator names only for methods *)
params      = param { "," param } ;
param       = [ "mut" ] NAME [ ":" type ] [ "=" expr ] ;
type_params = "[" CAPITALIZED_NAME { "," CAPITALIZED_NAME } "]" ;   (* T, Key, Value *)

class_decl  = "class" NAME [ "*" ] [ type_params ] [ "(" params ")" ] [ "is" NAME { "," NAME } ]
              [ NEWLINE INDENT { member NEWLINE } DEDENT ] ;
member      = NAME [ ":" type ] [ "=" expr ]         (* field *)
            | { "@" NAME } [ "fork" ] fn_decl        (* method *)
            | STRING ;                               (* docstring *)

interface_decl = "interface" NAME [ "*" ] [ "is" NAME { "," NAME } ]
                 NEWLINE INDENT { fn_decl NEWLINE } DEDENT ;
                                                     (* a method without a body is required *)
enum_decl   = "enum" NAME [ "*" ] NEWLINE INDENT { variant NEWLINE } { fn_decl NEWLINE } DEDENT ;
variant     = NAME [ "(" params ")" ] ;

use_stmt    = "use" dotted [ "as" NAME ]
            | "in" dotted "use" NAME [ "as" NAME ] { "," NAME [ "as" NAME ] } ;
dotted      = NAME { "." NAME } ;

select_stmt = "select" NEWLINE INDENT { select_arm NEWLINE } DEDENT ;
select_arm  = NAME "<:" expr block                   (* the first flow with a value wins *)
            | "after" expr block                     (* timeout in ms; `after` is only special here *)
            | "else" block ;                         (* nothing ready right now *)

for_stmt    = "for" NAME "in" expr block
            | "for" NAME "," NAME "in" expr block    (* the position (or map key) and the item *)
            | "for" NAME "," "(" NAME { "," NAME } ")" "in" expr block   (* position, item unpacked *)
            | "for" "(" NAME { "," NAME } ")" "in" expr block            (* each item unpacked *)
            | "for" NAME "," NAME "," NAME { "," NAME } "in" expr block  (* three or more: unpacked *)
            | "for" NAME "<:" expr { "," NAME "<:" expr } block ;

(* expressions, loosest first *)
expr        = catch { ":>" catch } ;
catch       = or [ "!" [ NAME ] block ] ;
or          = and { "or" and } ;
and         = not { "and" not } ;
not         = "not" not | compare ;
compare     = range { ( "==" | "!=" | "<" | "<=" | ">" | ">=" | "is" [ "not" ] | [ "not" ] "in" ) range } ;
range       = bitor [ ( ".." | "..=" ) bitor [ "by" bitor ] ] ;   (* `by`: the step *)
bitor       = bitxor { "|" bitxor } ;
bitxor      = bitand { "^" bitand } ;
bitand      = shift { "&" shift } ;
shift       = sum { ( "<<" | ">>" ) sum } ;
sum         = product { ( "+" | "-" ) product } ;
product     = unary { ( "*" | "/" | "%" | "//" ) unary } ;   (* // floor division *)
unary       = ( "-" | "~" ) unary | "&" postfix | power ;    (* -2 ** 2 is -(2 ** 2) *)
power       = postfix [ "**" unary ] ;
postfix     = primary postfix_ops ;
postfix_ops = { "(" args ")" | "." NAME | "[" index "]" | "?" }
              [ "->" lambda_body ] ;                 (* trailing lambda after a call *)
args        = [ arg { "," arg } [ "," ] ] ;
arg         = expr | "->" expr | NAME ":" expr ;      (* named arguments come last *)
index       = expr { "," expr }                      (* m[i, j] is m[(i, j)] *)
            | [ expr ] ( ".." | "..=" ) [ expr ] [ "by" expr ] ;   (* slices: xs[2..], xs[.. by -1] *)

primary     = NUMBER | STRING | FSTRING | "true" | "false" | "nil" | "self" | NAME
            | NAME "[" type { "," type } "]" [ "(" args ")" ]   (* flow[int](10) *)
            | "(" [ expr [ "," { expr "," } [ expr ] ] ] ")"    (* grouping / tuple *)
            | "[" [ expr { "," expr } [ "," ] ] "]"
            | "{" [ expr ":" expr { "," expr ":" expr } ] "}"   (* map *)
            | "{" expr { "," expr } "}"                         (* set *)
            | "fn" [ "(" params ")" ] [ ":" type ] ( "=" expr | block )   (* lambda *)
            | "->" expr
            | if_expr | match_expr
            | "fork" postfix [ "->" NAME block ]
            | "par" postfix ;                        (* run the call on another core *)

if_expr     = "if" expr block { "elif" expr block } [ "else" ( block | statement ) ] ;
match_expr  = "match" expr NEWLINE INDENT { pattern [ "if" expr ] block NEWLINE } DEDENT ;

pattern     = alt [ "as" NAME ] ;
alt         = pat { "|" pat } ;
pat         = "_" | literal [ ( ".." | "..=" ) literal ]
            | NAME                                   (* binds, lowercase *)
            | TYPE_NAME                              (* int, str, list, Error, ... *)
            | CLASS_NAME [ "(" pattern { "," pattern } ")" ]   (* also interfaces, enums *)
            | NAME "." NAME [ "(" pattern { "," pattern } ")" ]  (* enum variant: Shape.Rect(w, h) *)
            | NAME "." NAME { "." NAME }             (* value, e.g. Error.NotFound, Color.Red *)
            | "(" pattern { "," pattern } ")"
            | "[" [ ( pattern | ".." [ NAME ] ) { "," ( pattern | ".." [ NAME ] ) } ] "]"
            | "{" [ key ":" pattern { "," key ":" pattern } ] "}" ;   (* maps with these keys *)
key         = STRING | [ "-" ] NUMBER | "true" | "false" ;

type        = atom { "?" } [ "!" [ type ] ] ;        (* T ! E; T! is T ! Error *)
atom        = NAME [ "[" type { "," type } "]" ]
            | "(" type { "," type } ")"
            | "fn" [ "(" type { "," type } ")" ] [ "->" type ]   (* a function type *)
            | "&" [ "mut" ] type | "nil" ;
```

Literals: integers `42`, `1_000`, `0xff`, `0b101`; floats `1.5`, `2e10`; durations `500ms`,
`2s` (ints in milliseconds); strings `"..."`, `'...'`, `"""..."""`; f-strings
`f"...{expr[:spec]}..."` (`{{` and `}}` for braces); raw strings `r"..."`, `r'...'`,
`r"""..."""` (backslashes kept as written).

Keywords: `fn class interface enum if elif else while for in not and or is return match use
with fork par select defer true false nil break continue retry self as todo`. `tasks`, `mut`,
`after` and `by` are only special where shown above.
