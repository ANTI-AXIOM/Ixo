// Test Explorer integration: finds @test functions in .ixo files and runs them
// with `ixo test --json`, which prints one `@@ixo {...}` line per result.
const vscode = require('vscode');
const { spawn } = require('child_process');
const path = require('path');

const TEST_RE = /^@test[^\n]*\r?\n(?:\s*@[^\n]*\r?\n)*\s*fn\s+([A-Za-z_][A-Za-z0-9_]*)/gm;

function findTests(text) {
  const tests = [];
  let m;
  TEST_RE.lastIndex = 0;
  while ((m = TEST_RE.exec(text))) {
    const nameOffset = m.index + m[0].length - m[1].length;
    const line = text.slice(0, nameOffset).split('\n').length - 1;
    tests.push({ name: m[1], line });
  }
  return tests;
}

function createTestController(context, ixoPath) {
  const ctrl = vscode.tests.createTestController('ixoTests', 'Ixo');
  context.subscriptions.push(ctrl);

  function updateFile(uri, text) {
    const tests = findTests(text);
    const id = uri.toString();
    if (tests.length === 0) {
      ctrl.items.delete(id);
      return;
    }
    let file = ctrl.items.get(id);
    if (!file) {
      file = ctrl.createTestItem(id, vscode.workspace.asRelativePath(uri), uri);
      ctrl.items.add(file);
    }
    const children = tests.map((t) => {
      const item = ctrl.createTestItem(`${id}::${t.name}`, t.name, uri);
      item.range = new vscode.Range(t.line, 0, t.line, 0);
      return item;
    });
    file.children.replace(children);
  }

  async function discover() {
    const files = await vscode.workspace.findFiles('**/*.ixo', '**/{node_modules,build,archive,ixo_modules}/**');
    for (const uri of files) {
      const bytes = await vscode.workspace.fs.readFile(uri);
      updateFile(uri, Buffer.from(bytes).toString('utf8'));
    }
  }

  ctrl.resolveHandler = async (item) => {
    if (!item) await discover();
  };
  context.subscriptions.push(
    vscode.workspace.onDidChangeTextDocument((e) => {
      if (e.document.languageId === 'ixo') updateFile(e.document.uri, e.document.getText());
    }),
    vscode.workspace.onDidCreateFiles(() => discover()),
    vscode.workspace.onDidDeleteFiles((e) => e.files.forEach((u) => ctrl.items.delete(u.toString())))
  );

  // Runs `ixo test --json [--filter name] file` and reports the results.
  function runFile(run, fileItem, only, token) {
    return new Promise((resolve) => {
      const args = ['test', '--json'];
      if (only.length === 1) args.push('--filter', only[0].label);
      args.push(fileItem.uri.fsPath);
      const cwd = vscode.workspace.getWorkspaceFolder(fileItem.uri)?.uri.fsPath || path.dirname(fileItem.uri.fsPath);
      const proc = spawn(ixoPath(), args, { cwd });
      token.onCancellationRequested(() => proc.kill());
      const byName = new Map();
      fileItem.children.forEach((c) => byName.set(c.label, c));
      const wanted = new Set(only.map((t) => t.label));
      let buffer = '';
      let stderr = '';
      proc.stdout.on('data', (chunk) => {
        buffer += chunk.toString();
        let nl;
        while ((nl = buffer.indexOf('\n')) >= 0) {
          const line = buffer.slice(0, nl).replace(/\r$/, '');
          buffer = buffer.slice(nl + 1);
          if (!line.startsWith('@@ixo ')) {
            run.appendOutput(line + '\r\n');
            continue;
          }
          let ev;
          try {
            ev = JSON.parse(line.slice(6));
          } catch {
            continue;
          }
          if (ev.event === 'test') {
            const item = byName.get(ev.name);
            if (!item || (wanted.size && !wanted.has(ev.name))) continue;
            if (ev.status === 'passed') {
              run.passed(item, ev.ms);
            } else {
              const msg = new vscode.TestMessage(ev.message);
              const m = /^assertion failed: (.*) != (.*?)(?: \(|\n|$)/.exec(ev.message);
              if (m) {
                msg.actualOutput = m[1];
                msg.expectedOutput = m[2];
              }
              msg.location = new vscode.Location(item.uri, item.range);
              run.failed(item, msg, ev.ms);
            }
          } else if (ev.event === 'file' && ev.status === 'failed') {
            for (const item of only) run.errored(item, new vscode.TestMessage(ev.message));
          }
        }
      });
      proc.stderr.on('data', (chunk) => {
        stderr += chunk.toString();
        run.appendOutput(chunk.toString().replace(/\n/g, '\r\n'));
      });
      proc.on('error', (e) => {
        for (const item of only) run.errored(item, new vscode.TestMessage(`couldn't run ${ixoPath()}: ${e.message}`));
        resolve();
      });
      proc.on('close', () => resolve());
    });
  }

  async function runHandler(request, token) {
    const run = ctrl.createTestRun(request);
    const queue = new Map(); // file item -> tests
    const add = (item) => {
      if (request.exclude?.includes(item)) return;
      if (item.children.size > 0) {
        item.children.forEach(add);
        return;
      }
      const file = item.parent || item;
      if (!queue.has(file)) queue.set(file, []);
      queue.get(file).push(item);
    };
    if (request.include) request.include.forEach(add);
    else ctrl.items.forEach(add);
    for (const [file, tests] of queue) {
      if (token.isCancellationRequested) break;
      tests.forEach((t) => run.started(t));
      await runFile(run, file, tests.length === file.children.size ? [] : tests, token);
    }
    run.end();
  }

  ctrl.createRunProfile('Run', vscode.TestRunProfileKind.Run, runHandler, true);
  ctrl.createRunProfile(
    'Debug',
    vscode.TestRunProfileKind.Debug,
    async (request) => {
      const item = request.include?.[0];
      if (!item || !item.parent) return;
      await vscode.debug.startDebugging(vscode.workspace.getWorkspaceFolder(item.uri), {
        type: 'ixo',
        request: 'launch',
        name: `Debug test ${item.label}`,
        program: item.uri.fsPath,
        test: item.label,
      });
    },
    false
  );
  discover();
  return ctrl;
}

module.exports = { createTestController, findTests };
