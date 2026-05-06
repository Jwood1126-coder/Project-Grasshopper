import path from 'node:path'
import { defineConfig } from 'vite'
import preact from '@preact/preset-vite'

// Phase 2: plain Vite + Preact. Phase 7 will add vite-plugin-singlefile
// + a postbuild step to drop a gzipped app.html into firmware/main/assets/
// so it can be flashed to LittleFS.

export default defineConfig({
  plugins: [preact()],
  resolve: {
    alias: {
      '@proto': path.resolve(__dirname, '../proto/generated/types.ts'),
    },
  },
  build: {
    target: 'es2022',
    sourcemap: true,
  },
  server: {
    port: 5173,
  },
})
