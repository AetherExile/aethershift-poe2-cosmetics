import fs from "node:fs";
import path from "node:path";
import zlib from "node:zlib";

const inputRoot = path.resolve(process.argv[2] || "homebrew/RetroPapa/covers");
const outputRoot = path.resolve(process.argv[3] || "homebrew/RetroPapa/covers-native");
const TARGET_WIDTH = 360;
const TARGET_HEIGHT = 285;

function paeth(a, b, c) {
  const p = a + b - c;
  const pa = Math.abs(p - a);
  const pb = Math.abs(p - b);
  const pc = Math.abs(p - c);
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

export function decodePng(file) {
  const png = fs.readFileSync(file);
  if (png.subarray(0, 8).toString("hex") !== "89504e470d0a1a0a") {
    throw new Error(`${file}: not a PNG`);
  }

  let width;
  let height;
  let bitDepth;
  let colorType;
  let interlace;
  let palette;
  let transparency;
  const imageData = [];

  for (let offset = 8; offset < png.length;) {
    const length = png.readUInt32BE(offset);
    const type = png.toString("ascii", offset + 4, offset + 8);
    const data = png.subarray(offset + 8, offset + 8 + length);
    offset += 12 + length;

    if (type === "IHDR") {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      bitDepth = data[8];
      colorType = data[9];
      interlace = data[12];
    } else if (type === "PLTE") {
      palette = data;
    } else if (type === "tRNS") {
      transparency = data;
    } else if (type === "IDAT") {
      imageData.push(data);
    } else if (type === "IEND") {
      break;
    }
  }

  if (!width || !height || bitDepth !== 8 || interlace !== 0) {
    throw new Error(`${file}: only non-interlaced 8-bit PNGs are supported`);
  }

  const channels = colorType === 2 ? 3 : colorType === 3 ? 1 : colorType === 6 ? 4 : 0;
  if (!channels || (colorType === 3 && (!palette || palette.length % 3 !== 0))) {
    throw new Error(`${file}: unsupported PNG color type ${colorType}`);
  }

  const rowBytes = width * channels;
  const inflated = zlib.inflateSync(Buffer.concat(imageData));
  if (inflated.length !== (rowBytes + 1) * height) {
    throw new Error(`${file}: unexpected decompressed PNG size`);
  }

  const rows = Buffer.alloc(rowBytes * height);
  let inputOffset = 0;
  for (let y = 0; y < height; y += 1) {
    const filter = inflated[inputOffset++];
    const source = inflated.subarray(inputOffset, inputOffset + rowBytes);
    inputOffset += rowBytes;
    const rowOffset = y * rowBytes;
    const previousOffset = (y - 1) * rowBytes;

    for (let x = 0; x < rowBytes; x += 1) {
      const left = x >= channels ? rows[rowOffset + x - channels] : 0;
      const up = y > 0 ? rows[previousOffset + x] : 0;
      const upperLeft = y > 0 && x >= channels ? rows[previousOffset + x - channels] : 0;
      const value = source[x];
      if (filter === 0) rows[rowOffset + x] = value;
      else if (filter === 1) rows[rowOffset + x] = (value + left) & 255;
      else if (filter === 2) rows[rowOffset + x] = (value + up) & 255;
      else if (filter === 3) rows[rowOffset + x] = (value + Math.floor((left + up) / 2)) & 255;
      else if (filter === 4) rows[rowOffset + x] = (value + paeth(left, up, upperLeft)) & 255;
      else throw new Error(`${file}: unsupported PNG filter ${filter}`);
    }
  }

  const rgba = Buffer.alloc(width * height * 4);
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const sourceOffset = y * rowBytes + x * channels;
      const outputOffset = (y * width + x) * 4;
      if (colorType === 2) {
        rgba[outputOffset] = rows[sourceOffset];
        rgba[outputOffset + 1] = rows[sourceOffset + 1];
        rgba[outputOffset + 2] = rows[sourceOffset + 2];
        rgba[outputOffset + 3] = 255;
      } else if (colorType === 6) {
        rows.copy(rgba, outputOffset, sourceOffset, sourceOffset + 4);
      } else {
        const index = rows[sourceOffset];
        const paletteOffset = index * 3;
        if (paletteOffset + 2 >= palette.length) throw new Error(`${file}: invalid palette index ${index}`);
        rgba[outputOffset] = palette[paletteOffset];
        rgba[outputOffset + 1] = palette[paletteOffset + 1];
        rgba[outputOffset + 2] = palette[paletteOffset + 2];
        rgba[outputOffset + 3] = transparency?.[index] ?? 255;
      }
    }
  }

  return { width, height, rgba };
}

export function resizeImage(image, targetWidth, targetHeight) {
  const { width, height, rgba } = image;
  const targetRatio = targetWidth / targetHeight;
  const sourceRatio = width / height;
  let cropWidth = width;
  let cropHeight = height;
  let cropX = 0;
  let cropY = 0;

  if (sourceRatio > targetRatio) {
    cropWidth = height * targetRatio;
    cropX = (width - cropWidth) / 2;
  } else {
    cropHeight = width / targetRatio;
    cropY = (height - cropHeight) / 2;
  }

  const output = Buffer.alloc(targetWidth * targetHeight * 4);
  const sample = (x, y, channel) => {
    const clampedX = Math.max(0, Math.min(width - 1, x));
    const clampedY = Math.max(0, Math.min(height - 1, y));
    return rgba[(clampedY * width + clampedX) * 4 + channel];
  };

  for (let y = 0; y < targetHeight; y += 1) {
    const sourceY = cropY + ((y + 0.5) * cropHeight) / targetHeight - 0.5;
    const y0 = Math.floor(sourceY);
    const y1 = y0 + 1;
    const yWeight = sourceY - y0;
    for (let x = 0; x < targetWidth; x += 1) {
      const sourceX = cropX + ((x + 0.5) * cropWidth) / targetWidth - 0.5;
      const x0 = Math.floor(sourceX);
      const x1 = x0 + 1;
      const xWeight = sourceX - x0;
      const outputOffset = (y * targetWidth + x) * 4;
      for (let channel = 0; channel < 4; channel += 1) {
        const top = sample(x0, y0, channel) * (1 - xWeight) + sample(x1, y0, channel) * xWeight;
        const bottom = sample(x0, y1, channel) * (1 - xWeight) + sample(x1, y1, channel) * xWeight;
        output[outputOffset + channel] = Math.round(top * (1 - yWeight) + bottom * yWeight);
      }
      const alpha = output[outputOffset + 3] / 255;
      output[outputOffset] = Math.round(output[outputOffset] * alpha);
      output[outputOffset + 1] = Math.round(output[outputOffset + 1] * alpha);
      output[outputOffset + 2] = Math.round(output[outputOffset + 2] * alpha);
      output[outputOffset + 3] = 255;
    }
  }
  return output;
}

export function encodeBmp(rgba, targetWidth, targetHeight) {
  const rowBytes = targetWidth * 3;
  const paddedRowBytes = (rowBytes + 3) & ~3;
  const pixelOffset = 54;
  const output = Buffer.alloc(pixelOffset + paddedRowBytes * targetHeight);
  output.write("BM", 0, 2, "ascii");
  output.writeUInt32LE(output.length, 2);
  output.writeUInt32LE(pixelOffset, 10);
  output.writeUInt32LE(40, 14);
  output.writeInt32LE(targetWidth, 18);
  output.writeInt32LE(targetHeight, 22);
  output.writeUInt16LE(1, 26);
  output.writeUInt16LE(24, 28);
  output.writeUInt32LE(0, 30);
  output.writeUInt32LE(paddedRowBytes * targetHeight, 34);

  for (let outputY = 0; outputY < targetHeight; outputY += 1) {
    const sourceY = targetHeight - 1 - outputY;
    const rowOffset = pixelOffset + outputY * paddedRowBytes;
    for (let x = 0; x < targetWidth; x += 1) {
      const sourceOffset = (sourceY * targetWidth + x) * 4;
      const destinationOffset = rowOffset + x * 3;
      output[destinationOffset] = rgba[sourceOffset + 2];
      output[destinationOffset + 1] = rgba[sourceOffset + 1];
      output[destinationOffset + 2] = rgba[sourceOffset];
    }
  }
  return output;
}

function crc32(data) {
  let crc = 0xffffffff;
  for (const value of data) {
    crc ^= value;
    for (let bit = 0; bit < 8; bit += 1) {
      crc = (crc >>> 1) ^ (crc & 1 ? 0xedb88320 : 0);
    }
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function pngChunk(type, data) {
  const typeBytes = Buffer.from(type, "ascii");
  const chunk = Buffer.alloc(12 + data.length);
  chunk.writeUInt32BE(data.length, 0);
  typeBytes.copy(chunk, 4);
  data.copy(chunk, 8);
  chunk.writeUInt32BE(crc32(Buffer.concat([typeBytes, data])), 8 + data.length);
  return chunk;
}

export function encodePng(image) {
  const { width, height, rgba } = image;
  const rowBytes = width * 4;
  const scanlines = Buffer.alloc((rowBytes + 1) * height);
  for (let y = 0; y < height; y += 1) {
    const rowOffset = y * (rowBytes + 1);
    rgba.copy(scanlines, rowOffset + 1, y * rowBytes, (y + 1) * rowBytes);
  }

  const header = Buffer.alloc(13);
  header.writeUInt32BE(width, 0);
  header.writeUInt32BE(height, 4);
  header[8] = 8;
  header[9] = 6;
  const signature = Buffer.from("89504e470d0a1a0a", "hex");
  return Buffer.concat([
    signature,
    pngChunk("IHDR", header),
    pngChunk("IDAT", zlib.deflateSync(scanlines)),
    pngChunk("IEND", Buffer.alloc(0))
  ]);
}

function visit(directory) {
  for (const name of fs.readdirSync(directory)) {
    const source = path.join(directory, name);
    const stat = fs.statSync(source);
    if (stat.isDirectory()) visit(source);
    else if (name.toLowerCase().endsWith(".png")) {
      const relative = path.relative(inputRoot, source);
      const destination = path.join(outputRoot, relative.replace(/\.png$/i, ".bmp"));
      fs.mkdirSync(path.dirname(destination), { recursive: true });
      fs.writeFileSync(destination, encodeBmp(
        resizeImage(decodePng(source), TARGET_WIDTH, TARGET_HEIGHT),
        TARGET_WIDTH,
        TARGET_HEIGHT
      ));
      console.log(`Cover: ${relative} -> ${path.relative(outputRoot, destination)}`);
    }
  }
}

if (path.basename(process.argv[1] || "") === "convert-covers.mjs") visit(inputRoot);
