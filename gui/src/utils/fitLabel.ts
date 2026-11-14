// State labels wrap; tabs must fit on one line.
export function fitLabel(
  text: string,
  width: number,
  height: number,
  wrap = true,
) {
  for (let fontSize = 10; fontSize >= 8; fontSize--) {
    const lines: string[] = [];
    let line = "";
    let used = 0;
    let fits = true;

    for (const char of text) {
      const cw = char.charCodeAt(0) > 255 ? fontSize : fontSize * 0.6;
      if (used + cw > width) {
        if (!wrap || !line) {
          fits = false;
          break;
        }
        lines.push(line);
        line = char;
        used = cw;
      } else {
        line += char;
        used += cw;
      }
    }
    if (line) lines.push(line);
    if (fits && lines.length * (fontSize + 2) <= height) {
      return {
        fontSize,
        lineHeight: fontSize + 2,
        text: lines.join("\n"),
      };
    }
  }
  return null;
}
