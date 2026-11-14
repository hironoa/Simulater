import tailwindcss from '@tailwindcss/vite';
import react from '@vitejs/plugin-react';
import path from 'path';
import {defineConfig} from 'vite';
import { viteSingleFile } from 'vite-plugin-singlefile';

export default defineConfig({
  plugins: [react(), tailwindcss(), viteSingleFile()],
  resolve: {
    alias: {
      '@': path.resolve(__dirname, '.'),
    },
  },
  server: {
    // `npm run dev` のときは API をモックサーバ（npm run mock）か sim gui に中継する
    proxy: {
      '/api': { target: process.env.SIM_GUI_API ?? 'http://127.0.0.1:8765', changeOrigin: true },
    },
  },
});
