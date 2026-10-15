# Ixo for Visual Studio Code

Language support for the Ixo programming language:

- syntax highlighting (including f-strings, raw strings and semantic highlighting)
- errors and type errors as you type
- hover: types, signatures and documentation (docstrings and comments above declarations)
- go to definition (also across modules)
- find all references and rename (F2), across the whole workspace
- signature help while typing a call
- completion, with snippets, members after `.`, and every enum variant after `match`
- inlay hints: inferred types and parameter names
- quick fixes: add the methods an interface requires, add missing `match` arms, "did you
  mean", import an unknown name, export a private function, and update spellings from Ixo 0.5
  (one at a time or the whole file)
- document outline, format document (`ixo fmt`)
- tests in the Test Explorer, and **Run test** / **Debug test** links above `@test` functions
- debugging: breakpoints (with conditions, hit counts and log messages), stepping, call stack,
  variables you can change, a debug console that evaluates expressions, `par` workers as
  threads, stop on panic
- **Ixo: Run File** (Ctrl+F5 or the ▶ button), **Ixo: Run File and Watch for
  Changes**, **Ixo: Debug
  File**, **Ixo: Profile File** and **Ixo: Run @test Functions in File**

The extension runs `ixo lsp` and `ixo dap`. If `ixo` isn't on your `PATH`, set **ixo.path** in
the settings to the full path of `ixo.exe` (for example
`C:\Users\you\Documents\Ixo\build\release\ixo.exe`).

## Debugging

Press F5 in an `.ixo` file to debug it, or add a configuration to `.vscode/launch.json`:

```json
{
  "type": "ixo",
  "request": "launch",
  "name": "Debug main",
  "program": "${workspaceFolder}/main.ixo",
  "args": [],
  "stopOnEntry": false
}
```

Program output appears in the Debug Console. While debugging, `input()` returns `nil` (stdin
carries the debug protocol). The debug console evaluates any expression in the selected frame;
in a `par` worker's frames it shows variables and their fields and elements (`p.x`,
`items[0]`). Each `par` worker is listed as a thread with its own call stack.

To debug a test, use the **Debug test** link above it, or add `"test": "name"` to a launch
configuration.

## Building the extension

```sh
cd editors/vscode
npm install
npx @vscode/vsce package      # creates ixo-lang-1.0.0.vsix
code --install-extension ixo-lang-1.0.0.vsix
```
