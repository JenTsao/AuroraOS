#!/usr/bin/env python3
"""
scripts/gen_font.py

AuroraOS 位图字体打包工具：
  - 支持 1bpp (MONO_1BPP_MSB) 与 4bpp 抗锯齿灰度 (GRAY_4BPP)
  - 支持 ASCII + Unicode/中文字符集生成
  - 支持输出 C++ Flash FontDef 数组（含 ASCII 表与稀疏 UnicodeGlyphEntry 升序表）
  - 支持输出二进制 .afont ("AFNT" v1) 容器，用于 LittleFS / Flash 按需二分查找读取

示例：
  python scripts/gen_font.py --ttf C:/Windows/Fonts/msyh.ttc --size 16 --bpp 4 \
      --preset-zh-watch --out-afont zh16.afont
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    Image = None
    ImageDraw = None
    ImageFont = None

AFNT_MAGIC = 0x544E4641  # "AFNT" little-endian
AFNT_VERSION = 1
AFNT_HEADER_FMT = "<IHBBB3sIII"  # 24 bytes
AFNT_ENTRY_FMT = "<IIBBbbBBH"    # 16 bytes

PRESET_ZH_WATCH_CHARS = (
    "的一是在不了有和人这中大为上个国我以要他时来用们生到作地于出就分对成会可主发年动同工也能下过子说产种面而方后多定行学法所民得经十三之进着等部度家电力里如水化高自二理起小物现实加量都两体制机当使点从业本去把性好应开它合还因由其些然前外天政四日那社义事平形相全表间样与关各重新线内数正心反你明看原又么利比或但质气第向道命此变条只没结解问意建月公无系军很情者最立代想已通并提直题党程展五果料象员革位入常文总次品式活设及管特件长求老头基资边流路级少图山统接知较将组见计别她手角期根论运农指几九区强放决西被干做必战先回则任取据处队南给色光门即保治北造百规热领七海口东导器压志世金增争济阶油思术极交受联什认六共权收证改清己美再采转更单风切打白教速花带安场身车例真务具万每目至达走积示议声报斗完类八离华名确才科张信马节话米整空元况今集温传土许步群广石记需段研界拉林律叫且究观越织装影算低持音众书布复容儿须际商非验连断深难近矿千周委素技备半办青省列习响约支般史感劳便团往酸历市克何除消构府称太准精值号率族维划选标写存候毛亲快效斯院查江型眼王按格养易置派层片始却专状育厂京识适属圆包火住调满县局照参红细引听该铁价严"
    "心率步数电量时间日期星期一二三四五六七通知消息电话短信微信确认取消设置亮度音量连接断开蓝牙充电已完成睡眠运动卡路里距离配速表盘主题重启关机系统版本"
    "，。、：；！？（）【】“”‘’—…·℃%"
)


def clamp_i8(val: int) -> int:
    return max(-128, min(127, val))


def pack_glyph_pixels(pixels: list[int], bpp: int) -> bytearray:
    """将扁平像素亮度列表按 1bpp MSB 或 4bpp 半字节紧凑打包，与 drivers/display/bitmap_font.hpp 一致。"""
    packed = bytearray()
    if bpp == 4:
        byte_val = 0
        high_nibble = True
        for lum in pixels:
            cov4 = (int(lum) * 15 + 127) // 255
            cov4 = max(0, min(15, cov4))
            if high_nibble:
                byte_val = (cov4 & 0x0F) << 4
                high_nibble = False
            else:
                byte_val |= cov4 & 0x0F
                packed.append(byte_val)
                byte_val = 0
                high_nibble = True
        if not high_nibble:
            packed.append(byte_val)
        return packed

    current_byte = 0
    bit_pos = 0
    for lum in pixels:
        if lum > 0:
            current_byte |= 0x80 >> bit_pos
        bit_pos += 1
        if bit_pos == 8:
            packed.append(current_byte)
            current_byte = 0
            bit_pos = 0
    if bit_pos > 0:
        packed.append(current_byte)
    return packed


def generate_font_charset(
    font_path: str,
    size: int,
    name_suffix: str,
    chars: str,
    bpp: int = 4,
    spacing: int = 1,
) -> dict | None:
    if Image is None:
        raise RuntimeError("Pillow (PIL) is required to rasterize TTF fonts")
    if bpp not in (1, 4):
        raise ValueError(f"Unsupported bpp={bpp}; expected 1 or 4")

    font = ImageFont.truetype(font_path, size)
    ascent, descent = font.getmetrics()
    line_height = max(1, min(255, ascent + descent))

    unique_cps = sorted({ord(ch) for ch in chars if ord(ch) >= 0x20})
    bitmap_data = bytearray()
    entries = []

    pad = size
    canvas_w = max(32, size * 3)
    canvas_h = max(32, size * 3)
    mode = "L" if bpp == 4 else "1"
    fill_val = 255 if bpp == 4 else 1

    for cp in unique_cps:
        ch = chr(cp)
        img = Image.new(mode, (canvas_w, canvas_h), 0)
        draw = ImageDraw.Draw(img)
        draw.text((pad, pad), ch, font=font, fill=fill_val)
        bbox = img.getbbox()
        adv_px = max(1, min(255, int(round(font.getlength(ch)))))

        if bbox is None:
            entries.append(
                {
                    "codepoint": cp,
                    "width": min(255, adv_px),
                    "height": 0,
                    "x_offset": 0,
                    "y_offset": 0,
                    "x_advance": adv_px,
                    "bitmap_idx": len(bitmap_data),
                    "bitmap_bytes": 0,
                }
            )
            continue

        left, top, right, bottom = bbox
        gw = min(255, right - left)
        gh = min(255, bottom - top)
        cropped = img.crop((left, top, left + gw, top + gh))
        raw_pixels = (
            list(cropped.get_flattened_data())
            if hasattr(cropped, "get_flattened_data")
            else list(cropped.getdata())
        )
        packed = pack_glyph_pixels(raw_pixels, bpp)

        bmp_off = len(bitmap_data)
        bitmap_data.extend(packed)
        entries.append(
            {
                "codepoint": cp,
                "width": gw,
                "height": gh,
                "x_offset": clamp_i8(left - pad),
                "y_offset": clamp_i8(top - pad),
                "x_advance": adv_px,
                "bitmap_idx": bmp_off,
                "bitmap_bytes": len(packed),
            }
        )

    return {
        "suffix": name_suffix,
        "format": 1 if bpp == 4 else 0,
        "bpp": bpp,
        "line_height": line_height,
        "char_spacing": spacing,
        "entries": entries,
        "bitmap": bitmap_data,
    }


def build_afont_bytes(font_dict: dict) -> bytes:
    """将字体字典编码为 .afont ('AFNT' v1) 紧凑二进制流。"""
    entries = sorted(font_dict["entries"], key=lambda e: e["codepoint"])
    glyph_count = len(entries)
    index_offset = struct.calcsize(AFNT_HEADER_FMT)
    bitmap_offset = index_offset + glyph_count * struct.calcsize(AFNT_ENTRY_FMT)

    header = struct.pack(
        AFNT_HEADER_FMT,
        AFNT_MAGIC,
        AFNT_VERSION,
        font_dict["format"],
        font_dict["line_height"],
        font_dict["char_spacing"],
        b"\x00\x00\x00",
        glyph_count,
        index_offset,
        bitmap_offset,
    )

    out = bytearray(header)
    for e in entries:
        out.extend(
            struct.pack(
                AFNT_ENTRY_FMT,
                e["codepoint"],
                e["bitmap_idx"],
                e["width"],
                e["height"],
                e["x_offset"],
                e["y_offset"],
                e["x_advance"],
                0,
                e["bitmap_bytes"],
            )
        )
    out.extend(font_dict["bitmap"])
    return bytes(out)


def generate_font(font_path: str, size: int, name_suffix: str, bpp: int = 1):
    """兼容旧调用签名：生成 ASCII 32..126 字库字典。"""
    ascii_chars = "".join(chr(i) for i in range(32, 127))
    try:
        return generate_font_charset(font_path, size, name_suffix, ascii_chars, bpp=bpp, spacing=1)
    except Exception as e:
        print(f"Error loading font {font_path}: {e}")
        return None


def generate_cpp(fonts: list[dict], out_cpp: str, out_hpp: str, header_include: str = "font_engine.hpp"):
    hpp_content = [
        "#ifndef AURORA_FONT_DATA_HPP",
        "#define AURORA_FONT_DATA_HPP",
        "",
        f'#include "{header_include}"',
        "",
    ]
    cpp_content = [
        f'#include "{os.path.basename(out_hpp)}"',
        "",
    ]

    for f in fonts:
        s = f["suffix"]
        fmt_enum = "FontPixelFormat::GRAY_4BPP" if f["bpp"] == 4 else "FontPixelFormat::MONO_1BPP_MSB"
        by_cp = {e["codepoint"]: e for e in f["entries"]}

        has_ascii = all(cp in by_cp for cp in range(32, 127))
        start_char = 32 if has_ascii else 0
        end_char = 126 if has_ascii else 0
        ascii_entries = [by_cp[cp] for cp in range(32, 127)] if has_ascii else []
        unicode_entries = sorted(
            [e for e in f["entries"] if not (has_ascii and 32 <= e["codepoint"] <= 126)],
            key=lambda e: e["codepoint"],
        )

        cpp_content.append(f"const uint8_t font_bitmap_{s}[] = {{")
        line = "    "
        for i, b in enumerate(f["bitmap"]):
            line += f"0x{b:02X}, "
            if (i + 1) % 16 == 0:
                cpp_content.append(line)
                line = "    "
        if line != "    ":
            cpp_content.append(line)
        if not f["bitmap"]:
            cpp_content.append("    0x00")
        cpp_content.append("};")
        cpp_content.append("")

        ascii_sym = "nullptr"
        if ascii_entries:
            ascii_sym = f"font_glyphs_{s}"
            cpp_content.append(f"const GlyphInfo {ascii_sym}[] = {{")
            for g in ascii_entries:
                cpp_content.append(
                    f"    {{{g['width']}, {g['height']}, {g['x_offset']}, {g['y_offset']}, {g['bitmap_idx']}, {g['x_advance']}}},"
                )
            cpp_content.append("};")
            cpp_content.append("")

        uni_sym = "nullptr"
        if unicode_entries:
            uni_sym = f"font_unicode_{s}"
            cpp_content.append(f"const UnicodeGlyphEntry {uni_sym}[] = {{")
            for g in unicode_entries:
                cpp_content.append(
                    f"    {{0x{g['codepoint']:04X}u, {{{g['width']}, {g['height']}, {g['x_offset']}, {g['y_offset']}, {g['bitmap_idx']}, {g['x_advance']}}}}},"
                )
            cpp_content.append("};")
            cpp_content.append("")

        hpp_content.append(f"extern const FontDef font_{s};")
        cpp_content.append(f"const FontDef font_{s} = {{")
        cpp_content.append(f"    font_bitmap_{s},")
        cpp_content.append(f"    {ascii_sym},")
        cpp_content.append(f"    {f['line_height']},")
        cpp_content.append(f"    static_cast<char>({start_char}),")
        cpp_content.append(f"    static_cast<char>({end_char}),")
        cpp_content.append(f"    {fmt_enum},")
        cpp_content.append(f"    {f['char_spacing']},")
        cpp_content.append(f"    {uni_sym},")
        cpp_content.append(f"    {len(unicode_entries)}u")
        cpp_content.append("};")
        cpp_content.append("")

    hpp_content.append("#endif // AURORA_FONT_DATA_HPP\n")

    for path_str, content in ((out_cpp, cpp_content), (out_hpp, hpp_content)):
        out_dir = os.path.dirname(path_str)
        if out_dir:
            os.makedirs(out_dir, exist_ok=True)
        with open(path_str, "w", encoding="utf-8") as fp:
            fp.write("\n".join(content))


def main() -> int:
    if len(sys.argv) == 1:
        font_path = "C:/Windows/Fonts/arial.ttf"
        fonts = []
        for sz, suffix in ((20, "medium"), (40, "huge")):
            f = generate_font(font_path, sz, suffix, bpp=1)
            if f:
                fonts.append(f)
        out_dir = Path(__file__).resolve().parent.parent / "apps" / "watch"
        generate_cpp(fonts, str(out_dir / "font_data.cpp"), str(out_dir / "font_data.hpp"))
        print("Font data generated successfully.")
        return 0

    parser = argparse.ArgumentParser(description="AuroraOS 1bpp/4bpp & Unicode/CJK bitmap font generator")
    parser.add_argument("--ttf", required=True, help="Input TTF/TTC font file path")
    parser.add_argument("--size", type=int, default=16, help="Font pixel size (default: 16)")
    parser.add_argument("--bpp", type=int, choices=(1, 4), default=4, help="Bits per pixel (1 or 4, default: 4)")
    parser.add_argument("--spacing", type=int, default=1, help="Default char spacing in px (default: 1)")
    parser.add_argument("--name", default="custom", help="C symbol suffix for FontDef (default: custom)")
    parser.add_argument("--chars", default="", help="Extra explicit characters to include")
    parser.add_argument("--charset-file", help="UTF-8 text file whose unique characters will be included")
    parser.add_argument("--include-ascii", action="store_true", help="Include printable ASCII 32..126")
    parser.add_argument("--preset-zh-watch", action="store_true", help="Include common watch/RTOS Chinese characters")
    parser.add_argument("--out-afont", help="Output binary .afont file path for Flash/LittleFS")
    parser.add_argument("--out-cpp", help="Output C++ source path for Flash FontDef")
    parser.add_argument("--out-hpp", help="Output C++ header path for Flash FontDef")
    parser.add_argument(
        "--header-include",
        default="bitmap_font.hpp",
        help="Header included by generated .hpp (default: bitmap_font.hpp)",
    )
    args = parser.parse_args()

    chars = []
    if args.include_ascii or (not args.chars and not args.charset_file and not args.preset_zh_watch):
        chars.append("".join(chr(i) for i in range(32, 127)))
    if args.preset_zh_watch:
        chars.append(PRESET_ZH_WATCH_CHARS)
    if args.chars:
        chars.append(args.chars)
    if args.charset_file:
        chars.append(Path(args.charset_file).read_text(encoding="utf-8"))

    font_dict = generate_font_charset(args.ttf, args.size, args.name, "".join(chars), bpp=args.bpp, spacing=args.spacing)
    if font_dict is None:
        return 1

    if args.out_afont:
        raw = build_afont_bytes(font_dict)
        out_p = Path(args.out_afont)
        out_p.parent.mkdir(parents=True, exist_ok=True)
        out_p.write_bytes(raw)
        print(f"Wrote {out_p} ({len(font_dict['entries'])} glyphs, {len(raw)} bytes, {args.bpp}bpp)")

    if args.out_cpp or args.out_hpp:
        if not (args.out_cpp and args.out_hpp):
            parser.error("--out-cpp and --out-hpp must be provided together")
        generate_cpp([font_dict], args.out_cpp, args.out_hpp, header_include=args.header_include)
        print(f"Wrote C++ font pair: {args.out_cpp}, {args.out_hpp}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
