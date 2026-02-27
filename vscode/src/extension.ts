import * as path from 'path';
import * as fs from 'fs';
import * as http from 'http';
import * as vscode from 'vscode';
import {
	LanguageClient,
	LanguageClientOptions,
	ServerOptions,
	TransportKind,
} from 'vscode-languageclient/node';

// ============================================================================
// State
// ============================================================================

let client: LanguageClient | undefined;
let statusBarItem: vscode.StatusBarItem;
let compileBarItem: vscode.StatusBarItem;
let outputChannel: vscode.OutputChannel;

let hiseHost = 'localhost';
let hisePort = 1900;
let serverPath: string | undefined;
let lspArgs: string[] = [];
let scriptsFolder: string | undefined;

type ConnectionState = 'connected' | 'mismatch' | 'disconnected';
let connectionState: ConnectionState = 'disconnected';

/**
 * Store suggestions from LSP diagnostic data fields.
 * VS Code's vscode.Diagnostic doesn't expose the LSP `data` property,
 * so we intercept publishDiagnostics via middleware and cache suggestions
 * in a side map for the CodeActionProvider to look up.
 *
 * Key format: "uri|line|col|message"
 */
const diagnosticSuggestions = new Map<string, string[]>();

function makeDiagKey(uri: string, line: number, col: number, message: string): string {
	return `${uri}|${line}|${col}|${message}`;
}

// ============================================================================
// Utilities
// ============================================================================

/** Return the platform-specific binary name relative to the hise_lsp_server root. */
function getPlatformBinary(): string {
	switch (process.platform) {
		case 'win32':  return path.join('bin', 'windows', 'hise-lsp.exe');
		case 'darwin': return path.join('bin', 'macos', 'hise-lsp');
		default:       return path.join('bin', 'linux', 'hise-lsp');
	}
}

/**
 * Verify the workspace is a HISE project.
 * Activation was triggered by workspaceContains:project_info.xml,
 * but we also need Scripts/ to confirm.
 */
function isHiseProject(): boolean {
	for (const folder of vscode.workspace.workspaceFolders ?? []) {
		const root = folder.uri.fsPath;
		if (
			fs.existsSync(path.join(root, 'Scripts')) &&
			fs.existsSync(path.join(root, 'project_info.xml'))
		) {
			return true;
		}
	}
	return false;
}

/** Get the first workspace folder path. */
function getWorkspacePath(): string | undefined {
	const folders = vscode.workspace.workspaceFolders;
	if (!folders || folders.length === 0) return undefined;
	return folders[0].uri.fsPath;
}

/**
 * Resolve the hise-lsp binary path.
 * The extension lives at tools/hise_lsp_server/vscode/,
 * so the binary is at ../bin/<platform>/hise-lsp relative to the extension.
 */
function resolveServerPath(extensionPath: string): string | undefined {
	// Resolve symlinks/junctions so that ".." navigates the real filesystem
	const realExtensionPath = fs.realpathSync(extensionPath);
	const lspRoot = path.resolve(realExtensionPath, '..');
	const binPath = path.join(lspRoot, getPlatformBinary());

	if (fs.existsSync(binPath)) {
		return binPath;
	}

	vscode.window.showErrorMessage(
		`hise-lsp binary not found at ${binPath}. ` +
		'Make sure the hise_lsp_server binaries are built.'
	);
	return undefined;
}

/** Normalize a path for comparison: forward slashes, lowercase on Windows, no trailing separator. */
function normalizePath(p: string): string {
	let normalized = p.replace(/\\/g, '/').replace(/\/+$/, '');
	if (process.platform === 'win32') {
		normalized = normalized.toLowerCase();
	}
	return normalized;
}

// ============================================================================
// Code Actions (Quick Fixes)
// ============================================================================

class HiseCodeActionProvider implements vscode.CodeActionProvider {
	static readonly providedCodeActionKinds = [vscode.CodeActionKind.QuickFix];

	provideCodeActions(
		document: vscode.TextDocument,
		_range: vscode.Range,
		context: vscode.CodeActionContext,
	): vscode.CodeAction[] {
		const actions: vscode.CodeAction[] = [];

		for (const diag of context.diagnostics) {
			// Look up suggestions from our middleware cache
			const uriStr = document.uri.toString();
			const key = makeDiagKey(
				uriStr,
				diag.range.start.line,
				diag.range.start.character,
				diag.message,
			);
			const suggestions = diagnosticSuggestions.get(key);
			if (!suggestions || suggestions.length === 0) continue;

			// Use only the first suggestion
			const suggestion = suggestions[0];
			const line = document.lineAt(diag.range.start.line);
			const lineText = line.text;

			// Try to extract the broken identifier from the message.
			// Pattern: "Function / constant not found: Namespace.method"
			//   or:    "Function / constant not found: method"
			const notFoundMatch = diag.message.match(/not found:\s*([\w.]+)\s*$/);
			if (notFoundMatch) {
				const fullIdent = notFoundMatch[1]; // e.g. "Console.prins"
				const dotIdx = fullIdent.lastIndexOf('.');
				const brokenPart = dotIdx >= 0 ? fullIdent.substring(dotIdx + 1) : fullIdent;

				const fixedLine = lineText.replace(brokenPart, suggestion);
				if (fixedLine !== lineText) {
					const action = new vscode.CodeAction(
						`Replace '${brokenPart}' with '${suggestion}'`,
						vscode.CodeActionKind.QuickFix,
					);
					action.edit = new vscode.WorkspaceEdit();
					action.edit.replace(document.uri, line.range, fixedLine);
					action.diagnostics = [diag];
					action.isPreferred = true;
					actions.push(action);
					continue;
				}
			}

			// Fallback: show suggestion in action title (informational).
			// Covers "wrong signature" and other diagnostic patterns.
			const action = new vscode.CodeAction(
				`Suggested fix: ${suggestion}`,
				vscode.CodeActionKind.QuickFix,
			);
			action.diagnostics = [diag];
			actions.push(action);
		}

		return actions;
	}
}

// ============================================================================
// HISE REST API
// ============================================================================

interface HiseStatusResponse {
	success: boolean;
	project?: {
		name: string;
		projectFolder: string;
		scriptsFolder: string;
	};
}

interface HiseRecompileResponse {
	success: boolean;
	result?: string;
	errorMessage?: string;
	logs?: string[];
	errors?: Array<string | { errorMessage: string; callstack?: string[] }>;
}

interface ConnectionCheckResult {
	state: ConnectionState;
	projectName?: string;
}

/** Make a GET/POST request to the HISE REST API. Returns parsed JSON or null on failure. */
function hiseRequest(method: string, apiPath: string, body?: object): Promise<any | null> {
	return new Promise((resolve) => {
		const bodyStr = body ? JSON.stringify(body) : undefined;

		const headers: Record<string, string | number> = {
			'Connection': 'close',
		};
		if (bodyStr) {
			headers['Content-Type'] = 'application/json';
			headers['Content-Length'] = Buffer.byteLength(bodyStr);
		}

		const options: http.RequestOptions = {
			hostname: hiseHost,
			port: hisePort,
			path: apiPath,
			method: method,
			headers: headers,
			agent: false,    // disable connection pooling/keep-alive
			timeout: 30000,  // match HISE's compileTimeout
		};

		const req = http.request(options, (res) => {
			let data = '';
			res.on('data', (chunk: Buffer) => { data += chunk.toString(); });
			res.on('end', () => {
				try {
					resolve(JSON.parse(data));
				} catch {
					resolve(null);
				}
			});
		});

		req.on('error', () => resolve(null));
		req.on('timeout', () => { req.destroy(); resolve(null); });

		if (bodyStr) {
			req.write(bodyStr);
		}
		req.end();
	});
}

// ============================================================================
// Connection Status
// ============================================================================

async function checkHiseConnection(): Promise<ConnectionCheckResult> {
	const response = await hiseRequest('GET', '/api/status') as HiseStatusResponse | null;

	if (!response || !response.success) {
		return { state: 'disconnected' };
	}

	// Store scriptsFolder for clickable error links
	if (response.project?.scriptsFolder) {
		scriptsFolder = response.project.scriptsFolder;
	}

	// Check project folder matches workspace
	const workspacePath = getWorkspacePath();
	if (workspacePath && response.project?.projectFolder) {
		const hisePath = normalizePath(response.project.projectFolder);
		const vsPath = normalizePath(workspacePath);
		if (hisePath !== vsPath) {
			return { state: 'mismatch', projectName: response.project.name };
		}
	}

	return { state: 'connected', projectName: response.project?.name };
}

function updateStatusBar(state: ConnectionState, projectName?: string): void {
	connectionState = state;

	switch (state) {
		case 'connected':
			statusBarItem.text = '$(check) HISE';
			statusBarItem.backgroundColor = undefined;
			statusBarItem.tooltip = `Connected to HISE on port ${hisePort}`;
			compileBarItem.show();
			break;

		case 'mismatch':
			statusBarItem.text = '$(warning) HISE';
			statusBarItem.backgroundColor = new vscode.ThemeColor('statusBarItem.warningBackground');
			statusBarItem.tooltip = `HISE is running a different project${projectName ? ` ("${projectName}")` : ''}. Click to re-check.`;
			compileBarItem.hide();
			break;

		case 'disconnected':
			statusBarItem.text = '$(error) HISE';
			statusBarItem.backgroundColor = new vscode.ThemeColor('statusBarItem.errorBackground');
			statusBarItem.tooltip = `Cannot reach HISE on port ${hisePort}. Click to re-check.`;
			compileBarItem.hide();
			break;
	}

	statusBarItem.show();
}

async function refreshConnectionStatus(): Promise<void> {
	const prevState = connectionState;
	const result = await checkHiseConnection();

	updateStatusBar(result.state, result.projectName);

	// Handle LSP lifecycle on state change
	if (prevState !== result.state) {
		if (result.state === 'mismatch' && client) {
			// Wrong project — stop LSP, diagnostics would be meaningless
			await client.stop();
			client = undefined;
		} else if (result.state !== 'mismatch' && !client && serverPath) {
			// Reconnected or first time — start LSP
			await startLspClient();
		}
	}
}

// ============================================================================
// LSP Client
// ============================================================================

async function startLspClient(): Promise<void> {
	if (client || !serverPath) return;

	const serverOptions: ServerOptions = {
		run:   { command: serverPath, args: lspArgs, transport: TransportKind.stdio },
		debug: { command: serverPath, args: lspArgs, transport: TransportKind.stdio },
	};

	const clientOptions: LanguageClientOptions = {
		documentSelector: [
			{ scheme: 'file', language: 'hisescript' },
		],
		diagnosticCollectionName: 'hisescript',
		middleware: {
			handleDiagnostics(uri, diagnostics, next) {
				// Clear old suggestions for this URI
				const uriStr = uri.toString();
				for (const key of diagnosticSuggestions.keys()) {
					if (key.startsWith(uriStr + '|')) {
						diagnosticSuggestions.delete(key);
					}
				}

				// Extract suggestions from the raw LSP diagnostic data field.
				// The vscode-languageclient library converts LSP Diagnostics into
				// vscode.Diagnostic objects, but since we're in middleware we get
				// them after conversion. The `data` field is not on the vscode type,
				// so we cast to access it.
				for (const diag of diagnostics) {
					const data = (diag as any).data;
					if (data?.suggestions && Array.isArray(data.suggestions) && data.suggestions.length > 0) {
						const key = makeDiagKey(
							uriStr,
							diag.range.start.line,
							diag.range.start.character,
							diag.message,
						);
						diagnosticSuggestions.set(key, data.suggestions.map(String));
					}
				}

				// Pass diagnostics through to VS Code unchanged
				next(uri, diagnostics);
			},
		},
	};

	client = new LanguageClient(
		'hisescriptLsp',
		'HiseScript Language Server',
		serverOptions,
		clientOptions,
	);

	await client.start();
}

// ============================================================================
// Compile Command
// ============================================================================

/**
 * Format a HISE error message with a clickable file link for the VS Code output channel.
 * Input:  "test.js (7): Function / constant not found: Console.prnt"
 * Output: "D:/path/to/Scripts/test.js:7:1: Function / constant not found: Console.prnt"
 */
function formatErrorMessage(msg: string): string {
	// Match pattern: "filename (line): message"
	const match = msg.match(/^(.+?)\s*\((\d+)\):\s*(.+)$/);
	if (!match) return msg;

	const [, filename, line, message] = match;

	// Resolve to absolute path if we have the scripts folder
	if (scriptsFolder) {
		const absPath = path.join(scriptsFolder, filename).replace(/\\/g, '/');
		return `${absPath}:${line}:1: ${message}`;
	}

	// Fallback: just reformat with colon syntax
	return `${filename}:${line}:1: ${message}`;
}

async function compileHise(): Promise<void> {
	if (connectionState !== 'connected') {
		vscode.window.showWarningMessage('HISE is not connected. Cannot compile.');
		return;
	}

	// Save the active file before compiling
	const editor = vscode.window.activeTextEditor;
	if (editor?.document.isDirty) {
		await editor.document.save();
	}

	outputChannel.clear();
	outputChannel.appendLine('Compiling Interface...');

	const response = await hiseRequest('POST', '/api/recompile', {
		moduleId: 'Interface',
	}) as HiseRecompileResponse | null;

	if (!response) {
		outputChannel.appendLine('ERROR: No response from HISE');
		outputChannel.show(true);
		vscode.window.showErrorMessage('Compile failed: no response from HISE');
		return;
	}

	// Write logs to output channel
	if (response.logs && response.logs.length > 0) {
		for (const log of response.logs) {
			outputChannel.appendLine(log);
		}
	}

	if (response.success) {
		outputChannel.appendLine('Compile succeeded.');
		vscode.window.showInformationMessage('HISE compiled successfully');
	} else {
		// Write errors to output channel with clickable file links
		if (response.errors && response.errors.length > 0) {
			outputChannel.appendLine('');
			for (const err of response.errors) {
				const msg = typeof err === 'string' ? err : err.errorMessage;
				if (msg) {
					outputChannel.appendLine(formatErrorMessage(msg));
				}
			}
		}

		// Extract error message for the notification
		const firstError = response.errors?.[0];
		const firstErrorMsg = typeof firstError === 'string'
			? firstError
			: firstError?.errorMessage;
		const errorMsg = response.errorMessage || firstErrorMsg || 'Unknown error';

		outputChannel.appendLine('');
		outputChannel.appendLine('Compile failed.');
		outputChannel.show(true);  // auto-show on failure
		vscode.window.showErrorMessage(`Compile failed: ${errorMsg}`);
	}
}

// ============================================================================
// Activation / Deactivation
// ============================================================================

export async function activate(context: vscode.ExtensionContext) {
	// Double-check that this is a HISE project
	if (!isHiseProject()) {
		return;
	}

	serverPath = resolveServerPath(context.extensionPath);
	if (!serverPath) {
		return;
	}

	// Read config
	const config = vscode.workspace.getConfiguration('hisescript');
	hisePort = config.get<number>('server.port') ?? 1900;
	hiseHost = config.get<string>('server.host') ?? 'localhost';

	// Build CLI args — no --strict or --flat-suggestions for VS Code
	lspArgs = [];
	if (hisePort !== 1900) {
		lspArgs.push('--port', String(hisePort));
	}
	if (hiseHost !== 'localhost') {
		lspArgs.push('--host', hiseHost);
	}

	// Create output channel for compile results
	outputChannel = vscode.window.createOutputChannel('HiseScript');
	context.subscriptions.push(outputChannel);

	// Create status bar items
	statusBarItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 100);
	statusBarItem.command = 'hisescript.refreshStatus';
	context.subscriptions.push(statusBarItem);

	compileBarItem = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 99);
	compileBarItem.text = '$(play)';
	compileBarItem.tooltip = 'Compile HISE Script (F5)';
	compileBarItem.command = 'hisescript.compile';
	context.subscriptions.push(compileBarItem);

	// Register commands
	context.subscriptions.push(
		vscode.commands.registerCommand('hisescript.compile', compileHise),
		vscode.commands.registerCommand('hisescript.refreshStatus', refreshConnectionStatus),
	);

	// Register code action provider for quick fixes
	context.subscriptions.push(
		vscode.languages.registerCodeActionsProvider(
			'hisescript',
			new HiseCodeActionProvider(),
			{ providedCodeActionKinds: HiseCodeActionProvider.providedCodeActionKinds },
		),
	);

	// Initial connection check
	const initialCheck = await checkHiseConnection();
	updateStatusBar(initialCheck.state, initialCheck.projectName);

	// Start LSP unless project mismatch
	if (initialCheck.state !== 'mismatch') {
		await startLspClient();
	}
}

export async function deactivate(): Promise<void> {
	if (client) {
		await client.stop();
		client = undefined;
	}
}
