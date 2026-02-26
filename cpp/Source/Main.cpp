#include <JuceHeader.h>

#if JUCE_WINDOWS
#include <io.h>
#include <fcntl.h>
#endif

using namespace juce;

// ============================================================================
// Section 1: JSON-RPC Framing
// ============================================================================

// All logging goes to stderr (stdout is the LSP channel)
static void log(const String& msg)
{
	std::cerr << msg.toStdString() << std::endl;
}

// When true, all diagnostic severities are promoted to Error (1)
// so that agent platforms like OpenCode (which filter out non-errors) see everything.
static bool strictMode = false;

// Read a single LSP message from stdin.
// Format: "Content-Length: N\r\n\r\n{...json...}"
// Returns empty var on EOF.
static var readMessage()
{
	MemoryOutputStream line;
	int contentLength = -1;

	// Read headers
	while (true)
	{
		int c = std::cin.get();
		if (c == EOF) return {};

		if (c == '\n')
		{
			auto header = line.toString().trimEnd(); // remove \r if present

			if (header.isEmpty())
				break; // empty line = end of headers

			if (header.startsWith("Content-Length:"))
				contentLength = header.fromFirstOccurrenceOf(":", false, false).trim().getIntValue();

			line.reset();
		}
		else
		{
			line.writeByte((char)c);
		}
	}

	if (contentLength <= 0) return {};

	// Read body
	std::vector<char> buffer(contentLength);
	std::cin.read(buffer.data(), contentLength);
	if (std::cin.gcount() != contentLength) return {};

	return JSON::parse(String(buffer.data(), (size_t)contentLength));
}

// Write a JSON-RPC message to stdout.
static void writeMessage(const var& msg)
{
	MemoryOutputStream body;
	JSON::writeToStream(body, msg);

	auto bodySize = body.getDataSize();

	MemoryOutputStream frame;
	frame << "Content-Length: " << (int)bodySize << "\r\n\r\n";
	frame.write(body.getData(), bodySize);

	auto* data = (const char*)frame.getData();
	auto size = frame.getDataSize();

	std::cout.write(data, size);
	std::cout.flush();
}

// ============================================================================
// Section 2: HISE REST Client
// ============================================================================

struct HiseConnection
{
	String host = "localhost";
	int port = 1900;

	// Result of a diagnose call
	struct Result
	{
		bool connected = false;     // false = connection refused
		bool success = false;       // from JSON response
		String errorMessage;        // from JSON response or connection error
		var diagnostics;            // the diagnostics array from HISE
	};

	Result diagnoseFile(const String& filePath) const
	{
		DynamicObject::Ptr body = new DynamicObject();
		body->setProperty("filePath", filePath);

		auto url = URL("http://" + host + ":" + String(port) + "/api/diagnose_script")
			.withPOSTData(JSON::toString(var(body.get())));

		URL::InputStreamOptions options(URL::ParameterHandling::inAddress);

		auto stream = url.createInputStream(
			options.withExtraHeaders("Content-Type: application/json")
			       .withConnectionTimeoutMs(5000));

		if (stream == nullptr)
			return { false, false, "Cannot connect to HISE runtime (is HISE running on port "
			         + String(port) + "?)", {} };

		auto response = stream->readEntireStreamAsString();
		auto json = JSON::parse(response);

		if (!json.isObject())
			return { true, false, "Invalid response from HISE", {} };

		bool ok = (bool)json["success"];

		if (!ok)
			return { true, false, json["errorMessage"].toString(), {} };

		return { true, true, {}, json["diagnostics"] };
	}
};

// ============================================================================
// Section 3: Diagnostic Mapping
// ============================================================================

// Map HISE severity string to LSP DiagnosticSeverity integer
// LSP spec: 1=Error, 2=Warning, 3=Information, 4=Hint
static int mapSeverity(const String& severity)
{
	if (severity == "error")   return 1;
	if (severity == "warning") return 2;
	if (severity == "info")    return 3;
	if (severity == "hint")    return 4;
	return 1; // default to error
}

// Reverse map: LSP severity int to human-readable name (for strict mode prefixes)
static String severityName(int severity)
{
	if (severity == 2) return "warning";
	if (severity == 3) return "info";
	if (severity == 4) return "hint";
	return {};  // no prefix needed for errors
}

// Convert a single HISE diagnostic to an LSP Diagnostic object
static var makeLspDiagnostic(int line, int column, int severity,
                              const String& message, const String& source,
                              const var& suggestions)
{
	// LSP lines/columns are 0-based, HISE lines are 1-based
	int lspLine = jmax(0, line - 1);
	int lspCol = jmax(0, column - 1);

	// In strict mode, promote non-error severities to Error (1) and prefix
	// the message with the original severity so the agent knows the true level.
	auto finalMessage = message;
	auto finalSeverity = severity;

	if (strictMode && severity != 1)
	{
		finalMessage = "[" + severityName(severity) + "] " + message;
		finalSeverity = 1;
	}

	DynamicObject::Ptr range = new DynamicObject();
	DynamicObject::Ptr start = new DynamicObject();
	DynamicObject::Ptr end = new DynamicObject();
	start->setProperty("line", lspLine);
	start->setProperty("character", lspCol);
	end->setProperty("line", lspLine);
	end->setProperty("character", lspCol);
	range->setProperty("start", var(start.get()));
	range->setProperty("end", var(end.get()));

	DynamicObject::Ptr diag = new DynamicObject();
	diag->setProperty("range", var(range.get()));
	diag->setProperty("severity", finalSeverity);
	diag->setProperty("source", source.isNotEmpty() ? source : String("hisescript"));
	diag->setProperty("message", finalMessage);

	// Include suggestions in diagnostic data (agents can use this)
	if (suggestions.isArray() && suggestions.size() > 0)
	{
		DynamicObject::Ptr data = new DynamicObject();
		data->setProperty("suggestions", suggestions);
		diag->setProperty("data", var(data.get()));
	}

	return var(diag.get());
}

// Build LSP publishDiagnostics params from HISE response
static var mapHiseDiagnostics(const var& hiseDiagnostics, const String& uri)
{
	Array<var> lspDiags;

	if (hiseDiagnostics.isArray())
	{
		for (int i = 0; i < hiseDiagnostics.size(); i++)
		{
			auto& d = hiseDiagnostics[i];
			lspDiags.add(makeLspDiagnostic(
				(int)d["line"],
				(int)d["column"],
				mapSeverity(d["severity"].toString()),
				d["message"].toString(),
				d["source"].toString(),
				d["suggestions"]
			));
		}
	}

	DynamicObject::Ptr params = new DynamicObject();
	params->setProperty("uri", uri);
	params->setProperty("diagnostics", var(lspDiags));
	return var(params.get());
}

// Create a synthetic error diagnostic at line 1 (for connection errors, 404s, etc.)
static var makeSyntheticError(const String& uri, const String& message)
{
	Array<var> diags;
	diags.add(makeLspDiagnostic(1, 1, 1 /*Error*/, message, "hisescript", {}));

	DynamicObject::Ptr params = new DynamicObject();
	params->setProperty("uri", uri);
	params->setProperty("diagnostics", var(diags));
	return var(params.get());
}

// ============================================================================
// Section 4: LSP Handlers
// ============================================================================

class LspServer
{
public:
	HiseConnection hise;

	// Send a JSON-RPC notification (no id)
	void sendNotification(const String& method, const var& params)
	{
		DynamicObject::Ptr msg = new DynamicObject();
		msg->setProperty("jsonrpc", "2.0");
		msg->setProperty("method", method);
		msg->setProperty("params", params);
		writeMessage(var(msg.get()));
	}

	// Send a JSON-RPC response (with id)
	void sendResponse(const var& id, const var& result)
	{
		DynamicObject::Ptr msg = new DynamicObject();
		msg->setProperty("jsonrpc", "2.0");
		msg->setProperty("id", id);
		msg->setProperty("result", result);
		writeMessage(var(msg.get()));
	}

	// Send a JSON-RPC error response
	void sendError(const var& id, int code, const String& message)
	{
		DynamicObject::Ptr error = new DynamicObject();
		error->setProperty("code", code);
		error->setProperty("message", message);

		DynamicObject::Ptr msg = new DynamicObject();
		msg->setProperty("jsonrpc", "2.0");
		msg->setProperty("id", id);
		msg->setProperty("error", var(error.get()));
		writeMessage(var(msg.get()));
	}

	// Publish diagnostics for a document
	void publishDiagnostics(const var& params)
	{
		sendNotification("textDocument/publishDiagnostics", params);
	}

	// Diagnose a file and publish results
	void diagnoseAndPublish(const String& uri)
	{
		auto filePath = uriToPath(uri);

		log("Diagnosing: " + filePath);

		auto result = hise.diagnoseFile(filePath);

		if (!result.connected)
		{
			publishDiagnostics(makeSyntheticError(uri, result.errorMessage));
			return;
		}

		if (!result.success)
		{
			publishDiagnostics(makeSyntheticError(uri, result.errorMessage));
			return;
		}

		// Map HISE diagnostics to LSP format and publish
		publishDiagnostics(mapHiseDiagnostics(result.diagnostics, uri));
	}

	// --- Handler methods ---

	var handleInitialize(const var& /*params*/)
	{
		DynamicObject::Ptr capabilities = new DynamicObject();

		// Text document sync: open/close + full content on change + save
		DynamicObject::Ptr textDocSync = new DynamicObject();
		textDocSync->setProperty("openClose", true);
		textDocSync->setProperty("change", 1);  // 1 = Full (receive entire document on change)
		textDocSync->setProperty("save", true);
		capabilities->setProperty("textDocumentSync", var(textDocSync.get()));

		DynamicObject::Ptr result = new DynamicObject();
		result->setProperty("capabilities", var(capabilities.get()));

		// Server info
		DynamicObject::Ptr serverInfo = new DynamicObject();
		serverInfo->setProperty("name", "hise-lsp");
		serverInfo->setProperty("version", "1.0.0");
		result->setProperty("serverInfo", var(serverInfo.get()));

		return var(result.get());
	}

	void handleDidOpen(const var& params)
	{
		auto uri = params["textDocument"]["uri"].toString();
		if (uri.isNotEmpty())
			diagnoseAndPublish(uri);
	}

	void handleDidSave(const var& params)
	{
		auto uri = params["textDocument"]["uri"].toString();
		if (uri.isNotEmpty())
			diagnoseAndPublish(uri);
	}

	void handleDidChange(const var& params)
	{
		auto uri = params["textDocument"]["uri"].toString();
		if (uri.isNotEmpty())
			diagnoseAndPublish(uri);
	}

	void handleDidClose(const var& params)
	{
		auto uri = params["textDocument"]["uri"].toString();
		if (uri.isNotEmpty())
		{
			// Clear diagnostics for closed file
			DynamicObject::Ptr clearParams = new DynamicObject();
			clearParams->setProperty("uri", uri);
			clearParams->setProperty("diagnostics", var(Array<var>()));
			publishDiagnostics(var(clearParams.get()));
		}
	}

	// --- URI helpers ---

	// Convert file:///path/to/file.js to a local path
	static String uriToPath(const String& uri)
	{
		auto path = uri;

		if (path.startsWith("file:///"))
		{
			path = path.fromFirstOccurrenceOf("file:///", false, false);

			// On Windows: file:///C:/path -> C:/path
			// On Unix: file:///path -> /path
			#if ! JUCE_WINDOWS
			path = "/" + path;
			#endif
		}
		else if (path.startsWith("file://"))
		{
			path = path.fromFirstOccurrenceOf("file://", false, false);
		}

		// Decode percent-encoded characters (%20 -> space, etc.)
		path = URL::removeEscapeChars(path);

		// Normalize separators
		return path.replace("\\", "/");
	}
};

// ============================================================================
// Section 5: Dispatch Loop
// ============================================================================

static void runLspLoop(LspServer& server)
{
	bool running = true;

	while (running)
	{
		auto msg = readMessage();

		if (msg.isVoid())
		{
			// EOF on stdin — exit
			running = false;
			break;
		}

		auto method = msg["method"].toString();
		auto id = msg["id"];           // present for requests, absent for notifications
		auto params = msg["params"];
		bool isRequest = !id.isVoid();

		log(String(CharPointer_UTF8("\xe2\x86\x90")) + " " + method
		    + (isRequest ? " [" + id.toString() + "]" : ""));

		if (method == "initialize")
		{
			server.sendResponse(id, server.handleInitialize(params));
		}
		else if (method == "initialized")
		{
			// No-op notification
		}
		else if (method == "textDocument/didOpen")
		{
			server.handleDidOpen(params);
		}
		else if (method == "textDocument/didSave")
		{
			server.handleDidSave(params);
		}
		else if (method == "textDocument/didChange")
		{
			server.handleDidChange(params);
		}
		else if (method == "textDocument/didClose")
		{
			server.handleDidClose(params);
		}
		else if (method == "shutdown")
		{
			server.sendResponse(id, {});
		}
		else if (method == "exit")
		{
			running = false;
		}
		else if (isRequest)
		{
			// Unknown request — respond with MethodNotFound error
			server.sendError(id, -32601, "Method not found: " + method);
		}
		// Unknown notifications are silently ignored (per LSP spec)
	}
}

// ============================================================================
// Section 6: Unit Tests (--test mode)
// ============================================================================

static int runTests()
{
	int passed = 0, failed = 0;

	#define TEST(name, condition) \
		if (condition) { passed++; } \
		else { std::cerr << "FAIL: " << name << std::endl; failed++; }

	// --- uriToPath ---

	#if JUCE_WINDOWS
	TEST("uriToPath: Windows drive letter",
		LspServer::uriToPath("file:///C:/Users/chris/Scripts/test.js") == "C:/Users/chris/Scripts/test.js");

	TEST("uriToPath: percent encoding spaces",
		LspServer::uriToPath("file:///C:/path%20with%20spaces/test.js") == "C:/path with spaces/test.js");

	TEST("uriToPath: backslash normalization",
		LspServer::uriToPath("file:///C:\\path\\file.js") == "C:/path/file.js");

	TEST("uriToPath: plain path passthrough",
		LspServer::uriToPath("C:/path/file.js") == "C:/path/file.js");

	TEST("uriToPath: file:// two slashes",
		LspServer::uriToPath("file://C:/path/file.js") == "C:/path/file.js");

	TEST("uriToPath: uppercase drive percent-encoded colon",
		LspServer::uriToPath("file:///C%3A/Users/test.js") == "C:/Users/test.js");
	#else
	TEST("uriToPath: Unix absolute path",
		LspServer::uriToPath("file:///home/user/Scripts/test.js") == "/home/user/Scripts/test.js");

	TEST("uriToPath: Unix percent encoding",
		LspServer::uriToPath("file:///home/user/my%20project/test.js") == "/home/user/my project/test.js");
	#endif

	TEST("uriToPath: empty string",
		LspServer::uriToPath("") == "");

	// --- mapSeverity ---

	TEST("mapSeverity: error",   mapSeverity("error") == 1);
	TEST("mapSeverity: warning", mapSeverity("warning") == 2);
	TEST("mapSeverity: info",    mapSeverity("info") == 3);
	TEST("mapSeverity: hint",    mapSeverity("hint") == 4);
	TEST("mapSeverity: unknown defaults to error", mapSeverity("garbage") == 1);
	TEST("mapSeverity: empty defaults to error",   mapSeverity("") == 1);

	// --- makeLspDiagnostic ---

	{
		// Line/column 1-based to 0-based conversion
		auto d = makeLspDiagnostic(10, 5, 1, "test error", "api-validation", {});
		auto range = d["range"];
		TEST("makeLspDiagnostic: line conversion",
			(int)range["start"]["line"] == 9);
		TEST("makeLspDiagnostic: column conversion",
			(int)range["start"]["character"] == 4);
		TEST("makeLspDiagnostic: end matches start",
			(int)range["end"]["line"] == 9 && (int)range["end"]["character"] == 4);
		TEST("makeLspDiagnostic: severity preserved",
			(int)d["severity"] == 1);
		TEST("makeLspDiagnostic: message preserved",
			d["message"].toString() == "test error");
		TEST("makeLspDiagnostic: custom source preserved",
			d["source"].toString() == "api-validation");
	}

	{
		// Line 0 clamps to 0 (not -1)
		auto d = makeLspDiagnostic(0, 0, 2, "test", "", {});
		TEST("makeLspDiagnostic: line 0 clamp",
			(int)d["range"]["start"]["line"] == 0);
		TEST("makeLspDiagnostic: col 0 clamp",
			(int)d["range"]["start"]["character"] == 0);
	}

	{
		// Default source when empty
		auto d = makeLspDiagnostic(1, 1, 1, "msg", "", {});
		TEST("makeLspDiagnostic: empty source defaults to hisescript",
			d["source"].toString() == "hisescript");
	}

	{
		// Suggestions present
		Array<var> sugg;
		sugg.add("sendData");
		sugg.add("sendDataAsync");
		auto d = makeLspDiagnostic(1, 1, 1, "msg", "test", var(sugg));
		TEST("makeLspDiagnostic: suggestions in data",
			d["data"]["suggestions"].isArray() && d["data"]["suggestions"].size() == 2);
	}

	{
		// Suggestions absent (void)
		auto d = makeLspDiagnostic(1, 1, 1, "msg", "test", {});
		TEST("makeLspDiagnostic: no data when no suggestions",
			d["data"].isVoid());
	}

	{
		// Suggestions absent (empty array)
		auto d = makeLspDiagnostic(1, 1, 1, "msg", "test", var(Array<var>()));
		TEST("makeLspDiagnostic: no data when empty suggestions",
			d["data"].isVoid());
	}

	// --- mapHiseDiagnostics ---

	{
		// Empty diagnostics array
		auto result = mapHiseDiagnostics(var(Array<var>()), "file:///test.js");
		TEST("mapHiseDiagnostics: empty input gives empty output",
			result["diagnostics"].isArray() && result["diagnostics"].size() == 0);
		TEST("mapHiseDiagnostics: URI preserved",
			result["uri"].toString() == "file:///test.js");
	}

	{
		// Multiple diagnostics with mixed severities
		Array<var> hiseDiags;

		DynamicObject::Ptr d1 = new DynamicObject();
		d1->setProperty("line", 10);
		d1->setProperty("column", 3);
		d1->setProperty("severity", "error");
		d1->setProperty("source", "syntax");
		d1->setProperty("message", "Missing semicolon");
		hiseDiags.add(var(d1.get()));

		DynamicObject::Ptr d2 = new DynamicObject();
		d2->setProperty("line", 20);
		d2->setProperty("column", 8);
		d2->setProperty("severity", "warning");
		d2->setProperty("source", "callscope");
		d2->setProperty("message", "Unsafe call");
		hiseDiags.add(var(d2.get()));

		DynamicObject::Ptr d3 = new DynamicObject();
		d3->setProperty("line", 30);
		d3->setProperty("column", 1);
		d3->setProperty("severity", "hint");
		d3->setProperty("source", "language");
		d3->setProperty("message", "Use reg instead");
		hiseDiags.add(var(d3.get()));

		auto result = mapHiseDiagnostics(var(hiseDiags), "file:///multi.js");
		auto lspDiags = result["diagnostics"];
		TEST("mapHiseDiagnostics: 3 diags mapped",
			lspDiags.isArray() && lspDiags.size() == 3);
		TEST("mapHiseDiagnostics: first is error (severity 1)",
			(int)lspDiags[0]["severity"] == 1);
		TEST("mapHiseDiagnostics: second is warning (severity 2)",
			(int)lspDiags[1]["severity"] == 2);
		TEST("mapHiseDiagnostics: third is hint (severity 4)",
			(int)lspDiags[2]["severity"] == 4);
		TEST("mapHiseDiagnostics: line conversion on first",
			(int)lspDiags[0]["range"]["start"]["line"] == 9);
		TEST("mapHiseDiagnostics: source preserved",
			lspDiags[0]["source"].toString() == "syntax");
	}

	{
		// void input
		auto result = mapHiseDiagnostics({}, "file:///void.js");
		TEST("mapHiseDiagnostics: void input gives empty output",
			result["diagnostics"].isArray() && result["diagnostics"].size() == 0);
	}

	// --- makeSyntheticError ---

	{
		auto result = makeSyntheticError("file:///err.js", "Cannot connect to HISE");
		TEST("makeSyntheticError: URI preserved",
			result["uri"].toString() == "file:///err.js");
		TEST("makeSyntheticError: single diagnostic",
			result["diagnostics"].isArray() && result["diagnostics"].size() == 1);

		auto d = result["diagnostics"][0];
		TEST("makeSyntheticError: severity is Error (1)",
			(int)d["severity"] == 1);
		TEST("makeSyntheticError: line 0 (LSP 0-based)",
			(int)d["range"]["start"]["line"] == 0);
		TEST("makeSyntheticError: character 0",
			(int)d["range"]["start"]["character"] == 0);
		TEST("makeSyntheticError: message preserved",
			d["message"].toString() == "Cannot connect to HISE");
		TEST("makeSyntheticError: source is hisescript",
			d["source"].toString() == "hisescript");
	}

	// --- handleInitialize ---

	{
		LspServer s;
		auto result = s.handleInitialize({});

		auto caps = result["capabilities"];
		auto sync = caps["textDocumentSync"];
		TEST("handleInitialize: openClose is true",
			(bool)sync["openClose"] == true);
	TEST("handleInitialize: change is 1 (Full)",
		(int)sync["change"] == 1);
		TEST("handleInitialize: save is true",
			(bool)sync["save"] == true);

		auto info = result["serverInfo"];
		TEST("handleInitialize: server name",
			info["name"].toString() == "hise-lsp");
		TEST("handleInitialize: server version",
			info["version"].toString() == "1.0.0");
	}

	// --- strict mode ---

	{
		// Enable strict mode for these tests, restore after
		strictMode = true;

		// Warning promoted to Error, message gets [warning] prefix
		auto d1 = makeLspDiagnostic(5, 1, 2, "Unsafe call", "callscope", {});
		TEST("strict: warning promoted to severity 1",
			(int)d1["severity"] == 1);
		TEST("strict: warning message has prefix",
			d1["message"].toString() == "[warning] Unsafe call");

		// Hint promoted to Error, message gets [hint] prefix
		auto d2 = makeLspDiagnostic(10, 1, 4, "Use reg instead", "language", {});
		TEST("strict: hint promoted to severity 1",
			(int)d2["severity"] == 1);
		TEST("strict: hint message has prefix",
			d2["message"].toString() == "[hint] Use reg instead");

		// Info promoted to Error, message gets [info] prefix
		auto d3 = makeLspDiagnostic(15, 1, 3, "Some info", "api-validation", {});
		TEST("strict: info promoted to severity 1",
			(int)d3["severity"] == 1);
		TEST("strict: info message has prefix",
			d3["message"].toString() == "[info] Some info");

		// Error stays as Error, no prefix
		auto d4 = makeLspDiagnostic(20, 1, 1, "Missing semicolon", "syntax", {});
		TEST("strict: error stays severity 1",
			(int)d4["severity"] == 1);
		TEST("strict: error message unchanged",
			d4["message"].toString() == "Missing semicolon");

		strictMode = false;
	}

	{
		// Verify non-strict mode is unaffected (strictMode restored to false above)
		auto d = makeLspDiagnostic(5, 1, 2, "Unsafe call", "callscope", {});
		TEST("non-strict: warning stays severity 2",
			(int)d["severity"] == 2);
		TEST("non-strict: warning message unchanged",
			d["message"].toString() == "Unsafe call");
	}

	// --- severityName ---

	TEST("severityName: warning", severityName(2) == "warning");
	TEST("severityName: info",    severityName(3) == "info");
	TEST("severityName: hint",    severityName(4) == "hint");
	TEST("severityName: error returns empty", severityName(1).isEmpty());

	#undef TEST

	std::cerr << std::endl;

	if (failed == 0)
		std::cerr << "All " << passed << " tests passed." << std::endl;
	else
		std::cerr << passed << " passed, " << failed << " FAILED." << std::endl;

	return failed > 0 ? 1 : 0;
}

// ============================================================================
// Section 7: Main
// ============================================================================

int main(int argc, char* argv[])
{
	// Set stdin/stdout to binary mode on Windows (prevent \n -> \r\n translation)
	#if JUCE_WINDOWS
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
	#endif

	// Run unit tests if requested
	if (argc > 1 && String(argv[1]) == "--test")
		return runTests();

	LspServer server;

	// Parse command line args
	for (int i = 1; i < argc; i++)
	{
		String arg(argv[i]);

		if (arg == "--port" && i + 1 < argc)
			server.hise.port = String(argv[++i]).getIntValue();
		else if (arg == "--host" && i + 1 < argc)
			server.hise.host = String(argv[++i]);
		else if (arg == "--strict")
			strictMode = true;
	}

	log("hise-lsp v1.0.0 starting (HISE at " + server.hise.host + ":" + String(server.hise.port)
	    + (strictMode ? ", strict mode" : "") + ")");

	runLspLoop(server);

	return 0;
}
