// VS Code client for the Ixo language server (`ixo lsp`) and debug adapter (`ixo dap`).
const vscode = require('vscode');
const { LanguageClient } = require('vscode-languageclient/node');
const { createTestController } = require('./testing');

let client;

function ixoPath() {
  return vscode.workspace.getConfiguration('ixo').get('path') || 'ixo';
}

function sendToTerminal(args) {
  const term = vscode.window.terminals.find((t) => t.name === 'Ixo') || vscode.window.createTerminal('Ixo');
  term.show();
  const shell = (vscode.env.shell || '').toLowerCase();
  const quote = (s) => `"${s}"`;
  const call = shell.includes('powershell') || shell.includes('pwsh') ? '& ' : '';
  term.sendText(`${call}${quote(ixoPath())} ${args.map(quote).join(' ')}`);
}

function runInTerminal(args) {
  const editor = vscode.window.activeTextEditor;
  if (!editor || editor.document.languageId !== 'ixo') return;
  editor.document.save().then(() => sendToTerminal([...args, editor.document.fileName]));
}

// From the "Run test" / "Debug test" code lenses.
async function runTest(file, name) {
  await vscode.workspace.saveAll(false);
  sendToTerminal(['test', '--filter', name, file]);
}

async function debugTest(file, name) {
  await vscode.workspace.saveAll(false);
  const uri = vscode.Uri.file(file);
  vscode.debug.startDebugging(vscode.workspace.getWorkspaceFolder(uri), {
    type: 'ixo',
    request: 'launch',
    name: `Debug test ${name}`,
    program: file,
    test: name,
  });
}

async function startClient() {
  const serverOptions = { command: ixoPath(), args: ['lsp'] };
  const clientOptions = {
    documentSelector: [
      { scheme: 'file', language: 'ixo' },
      { scheme: 'untitled', language: 'ixo' },
    ],
  };
  client = new LanguageClient('ixo', 'Ixo Language Server', serverOptions, clientOptions);
  try {
    await client.start();
  } catch (e) {
    vscode.window.showErrorMessage(
      `Ixo: couldn't start the language server with "${ixoPath()} lsp". ` +
        `Set "ixo.path" in your settings to the ixo executable. (${e.message || e})`
    );
  }
}

// F5 without a launch.json: debug the current file.
const debugConfigProvider = {
  resolveDebugConfiguration(folder, config) {
    if (!config.type && !config.request && !config.name) {
      const editor = vscode.window.activeTextEditor;
      if (editor && editor.document.languageId === 'ixo') {
        config.type = 'ixo';
        config.name = 'Debug current Ixo file';
        config.request = 'launch';
        config.program = '${file}';
      }
    }
    if (!config.program) {
      return vscode.window.showInformationMessage('Open an .ixo file to debug it.').then(() => undefined);
    }
    if (!config.cwd && folder) config.cwd = folder.uri.fsPath;
    return config;
  },
};

const debugAdapterFactory = {
  createDebugAdapterDescriptor() {
    return new vscode.DebugAdapterExecutable(ixoPath(), ['dap']);
  },
};

function debugCurrentFile() {
  const editor = vscode.window.activeTextEditor;
  if (!editor || editor.document.languageId !== 'ixo') return;
  editor.document.save().then(() => {
    const folder = vscode.workspace.getWorkspaceFolder(editor.document.uri);
    vscode.debug.startDebugging(folder, {
      type: 'ixo',
      request: 'launch',
      name: 'Debug ' + editor.document.fileName.split(/[\\/]/).pop(),
      program: editor.document.fileName,
      cwd: folder ? folder.uri.fsPath : undefined,
    });
  });
}

async function activate(context) {
  context.subscriptions.push(
    vscode.debug.registerDebugConfigurationProvider('ixo', debugConfigProvider),
    vscode.debug.registerDebugAdapterDescriptorFactory('ixo', debugAdapterFactory),
    vscode.commands.registerCommand('ixo.run', () => runInTerminal([])),
    vscode.commands.registerCommand('ixo.debug', debugCurrentFile),
    vscode.commands.registerCommand('ixo.profile', () => runInTerminal(['--profile'])),
    vscode.commands.registerCommand('ixo.test', () => runInTerminal(['test'])),
    vscode.commands.registerCommand('ixo.watch', () => runInTerminal(['run', '--watch'])),
    vscode.commands.registerCommand('ixo.runTest', runTest),
    vscode.commands.registerCommand('ixo.debugTest', debugTest),
    vscode.commands.registerCommand('ixo.restartServer', async () => {
      if (client) await client.stop();
      await startClient();
    }),
    vscode.workspace.onDidChangeConfiguration(async (e) => {
      if (e.affectsConfiguration('ixo.path')) {
        if (client) await client.stop();
        await startClient();
      }
    })
  );
  createTestController(context, ixoPath);
  await startClient();
}

function deactivate() {
  return client ? client.stop() : undefined;
}

module.exports = { activate, deactivate };
