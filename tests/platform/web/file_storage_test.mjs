import {spawn} from "node:child_process";
import {mkdtemp, readFile, rm} from "node:fs/promises";
import {createServer} from "node:http";
import {tmpdir} from "node:os";
import {basename, join} from "node:path";

const [browser, moduleFile] = process.argv.slice(2);
const target = basename(moduleFile, ".js");
const assets = new Map([
  ["/", [new URL("file_storage.html", import.meta.url), "text/html"]],
  [`/${target}.js`, [moduleFile, "text/javascript"]],
  [`/${target}.wasm`, [moduleFile.replace(/\.js$/, ".wasm"), "application/wasm"]],
  [`/${target}.data`, [moduleFile.replace(/\.js$/, ".data"), "application/octet-stream"]],
]);

let report;
const reported = new Promise(resolve => { report = resolve; });
const server = createServer(async (request, response) => {
  if (request.method === "POST" && request.url === "/result") {
    request.setEncoding("utf8");
    let result = "";
    for await (const chunk of request) result += chunk;
    response.end();
    report(result);
    return;
  }
  const asset = assets.get(request.url);
  if (asset) {
    try {
      const [path, type] = asset;
      const contents = await readFile(path);
      response.writeHead(200, {"Content-Type": type});
      response.end(contents);
      return;
    } catch (error) {
      report(String(error));
    }
  }
  response.writeHead(404).end();
});
await new Promise((resolve, reject) => {
  server.once("error", reject);
  server.listen(0, "127.0.0.1", resolve);
});

const profile = await mkdtemp(join(tmpdir(), "huxerui-web-storage-"));
const chrome = spawn(browser, [
  "--headless", "--no-first-run", "--disable-background-networking", `--user-data-dir=${profile}`,
  `http://127.0.0.1:${server.address().port}/`,
], {stdio: ["ignore", "ignore", "inherit"]});
const exited = new Promise(resolve => {
  chrome.on("error", error => report(String(error)));
  chrome.on("close", (code, signal) => resolve(`browser exited before reporting a result (${signal ?? code})`));
});
let timer;
try {
  const timeout = new Promise(resolve => {
    timer = setTimeout(() => resolve("browser did not report a result"), 30_000);
  });
  const result = await Promise.race([reported, exited, timeout]);
  if (result !== "PASS") throw new Error(result);
  console.log("PASS: persistent write policy before and after mounting, MEMFS random write, and IndexedDB reload");
} finally {
  clearTimeout(timer);
  chrome.kill();
  await exited;
  server.closeAllConnections();
  server.close();
  await rm(profile, {recursive: true, force: true, maxRetries: 3});
}
