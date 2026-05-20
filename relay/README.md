# grasshopper-relay

Debug telescope for Project Grasshopper. Bun + Hono, deployed to Railway.

## Runtime: Bun required

The relay uses Bun-native APIs (`Bun.serve`, `ServerWebSocket<>`) and
runs as `bun run src/server.ts`. Node won't run it — the WS upgrade
path and the binary preview frame handler are tied to Bun's HTTP
server.

```bash
bun run dev      # hot-reload local dev
bun run start    # production
```

## Typecheck: Node-compatible

TypeScript validation does NOT require Bun. The `typecheck` script
invokes `tsc --noEmit`, which works under either runtime as long as
the devDependencies are installed:

```bash
npm install       # or: bun install
npm run typecheck # or: bun run typecheck
# or directly:
./node_modules/.bin/tsc --noEmit
```

This split matters during code review: someone reading the diff can
sanity-check types with plain `npm`-tooling even if Bun isn't on
their machine. They just can't *run* the server without Bun.

## Inner JS sanity check (optional)

The user-facing HTML lives inline in `src/user_ui.ts` as a single
template literal. TypeScript checks the literal but doesn't parse the
inner `<script>` — so syntax errors inside that JS slip past `tsc`.
For a real parse, decode the template-literal escapes and pipe to
Node's `--check`:

```bash
node -e "
const fs = require('fs');
const src = fs.readFileSync('src/user_ui.ts', 'utf8');
const m = src.match(/USER_UI_HTML = \`([\s\S]+)\`/);
const decoded = m[1]
  .replace(/\\\\\`/g, '\`')
  .replace(/\\\\\\\\/g, '\\\\')
  .replace(/\\\\\\\$/g, '\\\$');
const a = decoded.indexOf('<script>');
const b = decoded.lastIndexOf('</script>');
fs.writeFileSync('/tmp/inner.js', decoded.substring(a + 8, b));
" && node --check /tmp/inner.js
```

## Notes

- Backticks inside the `USER_UI_HTML` template literal are forbidden
  everywhere — including JS comments. They terminate the literal
  silently and `tsc` emits cryptic "',' expected" errors several
  lines later. Use double-quotes for inline references in comments.
- The relay is deployed to
  `https://project-grasshopper-production.up.railway.app` on every
  push to `main`. Railway picks `bun run start` from the Procfile /
  package.json automatically.
