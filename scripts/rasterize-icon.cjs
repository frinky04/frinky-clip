// Run from the repository root after:
// npm install --prefix .deps/icon-tools --no-audit --no-fund @resvg/resvg-js@2.6.2
// The generated ICO is checked in; ordinary builds do not need Node.js.
const fs = require('node:fs');
const path = require('node:path');
const root = path.resolve(__dirname, '..');
const { Resvg } = require(path.join(root, '.deps/icon-tools/node_modules/@resvg/resvg-js'));
const svg = fs.readFileSync(path.join(root, 'icon.svg'));
const sizes = [16, 20, 24, 32, 40, 48, 64, 128, 256];
const frames = sizes.map(size => new Resvg(svg, {
    fitTo: { mode: 'width', value: size }, font: { loadSystemFonts: false }
}).render().asPng());

// Windows ICO directory followed by one lossless PNG per size.
const directory = Buffer.alloc(6 + 16 * frames.length);
directory.writeUInt16LE(1, 2);
directory.writeUInt16LE(frames.length, 4);
let offset = directory.length;
frames.forEach((png, i) => {
    const entry = 6 + 16 * i;
    directory[entry] = directory[entry + 1] = sizes[i] % 256;
    directory.writeUInt16LE(1, entry + 4);
    directory.writeUInt16LE(32, entry + 6);
    directory.writeUInt32LE(png.length, entry + 8);
    directory.writeUInt32LE(offset, entry + 12);
    offset += png.length;
});
fs.writeFileSync(path.join(root, 'icon.ico'), Buffer.concat([directory, ...frames]));
