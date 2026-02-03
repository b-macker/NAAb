import * as path from 'path';
import { workspace, ExtensionContext } from 'vscode';

import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    TransportKind
} from 'vscode-languageclient/node';

let client: LanguageClient;

export function activate(context: ExtensionContext) {
    // Get LSP server path from configuration
    const config = workspace.getConfiguration('naab');
    const serverPath = config.get<string>('lsp.serverPath') || 'naab-lsp';

    console.log('NAAb extension activating with server:', serverPath);

    // Server options
    const serverOptions: ServerOptions = {
        command: serverPath,
        args: [],
        transport: TransportKind.stdio
    };

    // Client options
    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: 'file', language: 'naab' }],
        synchronize: {
            fileEvents: workspace.createFileSystemWatcher('**/*.naab')
        }
    };

    // Create language client
    client = new LanguageClient(
        'naab',
        'NAAb Language Server',
        serverOptions,
        clientOptions
    );

    // Start client
    client.start();

    console.log('NAAb language client started');
}

export function deactivate(): Thenable<void> | undefined {
    if (!client) {
        return undefined;
    }
    return client.stop();
}
