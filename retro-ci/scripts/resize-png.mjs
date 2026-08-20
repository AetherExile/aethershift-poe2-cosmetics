import fs from "node:fs";
import path from "node:path";
import { decodePng, encodePng, resizeImage } from "./convert-covers.mjs";

const [source, destination, widthText, heightText] = process.argv.slice(2);
const width = Number(widthText);
const height = Number(heightText);

if (!source || !destination || !Number.isInteger(width) || !Number.isInteger(height) || width < 1 || height < 1) {
  throw new Error("usage: node resize-png.mjs input.png output.png width height");
}

const image = resizeImage(decodePng(source), width, height);
fs.mkdirSync(path.dirname(destination), { recursive: true });
fs.writeFileSync(destination, encodePng({ width, height, rgba: image }));
console.log(`PNG: ${source} -> ${destination} (${width}x${height})`);
