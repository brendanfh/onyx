import * as vscode from "vscode";
import TextmateLanguageService from 'vscode-textmate-languageservice';

import * as vslc2 from "vscode-languageclient";
import * as vslc from "vscode-languageclient/node";

let client: vslc.LanguageClient;

export async function activate(context: vscode.ExtensionContext) {
	let console = vscode.window.createOutputChannel("Onyx Extension");
	console.appendLine("Starting Onyx Extension");

	const selector: vscode.DocumentSelector = 'onyx';
	const textmateService = new TextmateLanguageService(selector, context);
	const documentSymbolProvider = await textmateService.createDocumentSymbolProvider();
	const workspaceSymbolProvider = await textmateService.createWorkspaceSymbolProvider();
	const definitionProvider = await textmateService.createDefinitionProvider();

	context.subscriptions.push(vscode.languages.registerDocumentSymbolProvider(selector, documentSymbolProvider));
	context.subscriptions.push(vscode.languages.registerWorkspaceSymbolProvider(workspaceSymbolProvider));
	context.subscriptions.push(vscode.languages.registerDefinitionProvider(selector, definitionProvider));

	let serverOptions: vslc.ServerOptions = {
		command: "onyx-lsp",
		transport: vslc.TransportKind.stdio,
	};

	let clientOptions: vslc.LanguageClientOptions = {
		documentSelector: [
			{ scheme: "file", language: "onyx" },
		],
		connectionOptions: {
			cancellationStrategy: null,
			maxRestartCount: 5
		}
	};

	client = new vslc.LanguageClient("onyx-lsp", serverOptions, clientOptions);

	client.start();

	console.appendLine("Onyx Extension loaded.");
}

export function deactivate(): Thenable<void> | undefined {
	if (!client) {
		return undefined;
	}

	return client.stop();	
}
