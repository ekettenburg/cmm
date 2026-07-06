// C-- (cmm) VS Code extension: live diagnostics by running `cmmc check`.
const vscode = require("vscode");
const cp = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");

let diagnostics;
let debounceTimer;

function config() {
  return vscode.workspace.getConfiguration("cmm");
}

// Parse compiler output like:  Foo.cmm:3:7: error: undefined name 'x'
const LINE_RE = /^(.*?):(\d+):(\d+):\s*(error|warning):\s*(.*)$/;

function parseDiagnostics(output) {
  const out = [];
  for (const raw of output.split(/\r?\n/)) {
    const m = LINE_RE.exec(raw.trim());
    if (!m) continue;
    const line = Math.max(0, parseInt(m[2], 10) - 1);
    const col = Math.max(0, parseInt(m[3], 10) - 1);
    const sev =
      m[4] === "warning"
        ? vscode.DiagnosticSeverity.Warning
        : vscode.DiagnosticSeverity.Error;
    // Highlight from the reported column to the end of that word/line.
    const range = new vscode.Range(line, col, line, col + 1);
    const d = new vscode.Diagnostic(range, m[5], sev);
    d.source = "cmmc";
    out.push({ range, diag: d });
  }
  return out;
}

function lint(document) {
  if (!document || document.languageId !== "cmm") return;
  if (!config().get("lint.enable", true)) {
    diagnostics.delete(document.uri);
    return;
  }
  const cmmc = config().get("compilerPath", "cmmc");

  // Write current (possibly unsaved) content to a temp file that keeps the
  // same basename, so the classname==filename rule is satisfied.
  const base = path.basename(document.fileName) || "Main.cmm";
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "cmm-lint-"));
  const tmp = path.join(dir, base);
  try {
    fs.writeFileSync(tmp, document.getText());
  } catch (e) {
    return;
  }

  cp.execFile(cmmc, ["check", tmp], { timeout: 10000 }, (err, stdout, stderr) => {
    try { fs.rmSync(dir, { recursive: true, force: true }); } catch (e) {}

    if (err && err.code === "ENOENT") {
      // compiler not found — surface a single actionable message once
      const d = new vscode.Diagnostic(
        new vscode.Range(0, 0, 0, 1),
        `cmmc not found (set "cmm.compilerPath"). Diagnostics disabled.`,
        vscode.DiagnosticSeverity.Warning
      );
      d.source = "cmmc";
      diagnostics.set(document.uri, [d]);
      return;
    }

    const text = (stderr || "") + "\n" + (stdout || "");
    const parsed = parseDiagnostics(text);
    diagnostics.set(
      document.uri,
      parsed.map((p) => p.diag)
    );
  });
}

function scheduleLint(document) {
  const when = config().get("lint.run", "onType");
  if (when !== "onType") return;
  clearTimeout(debounceTimer);
  debounceTimer = setTimeout(() => lint(document), 300);
}

function activate(context) {
  diagnostics = vscode.languages.createDiagnosticCollection("cmm");
  context.subscriptions.push(diagnostics);

  if (vscode.window.activeTextEditor) {
    lint(vscode.window.activeTextEditor.document);
  }

  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument((doc) => lint(doc)),
    vscode.workspace.onDidSaveTextDocument((doc) => lint(doc)),
    vscode.workspace.onDidChangeTextDocument((e) => scheduleLint(e.document)),
    vscode.workspace.onDidCloseTextDocument((doc) => diagnostics.delete(doc.uri)),
    vscode.window.onDidChangeActiveTextEditor((ed) => {
      if (ed) lint(ed.document);
    })
  );
}

function deactivate() {
  if (diagnostics) diagnostics.clear();
}

module.exports = { activate, deactivate };
