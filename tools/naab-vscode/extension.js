const { LanguageClient, TransportKind } = require('vscode-languageclient/node');
const vscode = require('vscode');

let client;

function activate(context) {
    const config = vscode.workspace.getConfiguration('naab');
    const lspPath = config.get('lspPath', 'naab-lsp');
    const logLevel = config.get('logLevel', 'INFO');

    const serverOptions = {
        command: lspPath,
        transport: TransportKind.stdio,
        options: {
            env: { ...process.env, NAAB_LSP_LOG_LEVEL: logLevel }
        }
    };

    const clientOptions = {
        documentSelector: [{ scheme: 'file', language: 'naab' }],
        synchronize: {
            fileEvents: vscode.workspace.createFileSystemWatcher('**/*.naab')
        }
    };

    client = new LanguageClient(
        'naab-lsp',
        'NAAb Language Server',
        serverOptions,
        clientOptions
    );

    client.start();
}

function deactivate() {
    if (client) {
        return client.stop();
    }
}

module.exports = { activate, deactivate };
