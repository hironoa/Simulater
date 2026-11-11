// dist/index.html を、sim がそのまま配る単一 HTML の名前（dist/sim_gui.html）にする。
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const dist = path.join(path.dirname(fileURLToPath(import.meta.url)), "..", "dist");
const from = path.join(dist, "index.html");
const to = path.join(dist, "sim_gui.html");
fs.renameSync(from, to);
console.log(`postbuild: ${path.relative(process.cwd(), to)} (${fs.statSync(to).size.toLocaleString()} bytes)`);
