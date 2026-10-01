// Assembles the static site into web/dist:
//   src/*                 -> dist/
//   build-web/bfweb.*     -> dist/wasm/   (BF_WASM_DIR overrides the build directory)
//   public/corpus/**      -> dist/corpus/
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const web = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const root = path.dirname(web);
const dist = path.join(web, 'dist');
const wasmDir = process.env.BF_WASM_DIR || path.join(root, 'build-web');

for (const f of ['bfweb.mjs', 'bfweb.wasm'])
  if (!fs.existsSync(path.join(wasmDir, f))) {
    console.error(`missing ${path.join(wasmDir, f)}: run "npm run wasm" first (see web/README.md)`);
    process.exit(1);
  }

fs.rmSync(dist, { recursive: true, force: true });
fs.mkdirSync(path.join(dist, 'wasm'), { recursive: true });
fs.cpSync(path.join(web, 'src'), dist, { recursive: true });
for (const f of ['bfweb.mjs', 'bfweb.wasm']) fs.copyFileSync(path.join(wasmDir, f), path.join(dist, 'wasm', f));
if (fs.existsSync(path.join(web, 'public'))) fs.cpSync(path.join(web, 'public'), dist, { recursive: true });
fs.writeFileSync(path.join(dist, '.nojekyll'), '');

let total = 0;
const walk = (d) => fs.readdirSync(d, { withFileTypes: true }).forEach((e) => {
  const p = path.join(d, e.name);
  if (e.isDirectory()) walk(p); else total += fs.statSync(p).size;
});
walk(dist);
const size = (p) => (fs.statSync(path.join(dist, p)).size / 1024).toFixed(0) + ' KiB';
console.log(`web/dist: ${(total / 1048576).toFixed(1)} MiB total; wasm ${size('wasm/bfweb.wasm')}, glue ${size('wasm/bfweb.mjs')}, app ${size('app.js')}`);
