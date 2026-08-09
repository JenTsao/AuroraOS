import os
from PIL import ImageFont, ImageDraw, Image

def generate_font(font_path, size, name_suffix):
    try:
        font = ImageFont.truetype(font_path, size)
    except Exception as e:
        print(f"Error loading font {font_path}: {e}")
        return None

    chars = [chr(i) for i in range(32, 127)]
    glyphs = []
    bitmap_data = []

    ascent, descent = font.getmetrics()
    line_height = ascent + descent

    bitmap_idx = 0
    for c in chars:
        w, h = (size * 2, size * 2)
        img = Image.new('1', (w, h), 0)
        draw = ImageDraw.Draw(img)
        draw.text((size, size), c, font=font, fill=1)
        bbox = img.getbbox()
        
        if bbox is None:
            adv = font.getlength(c)
            glyphs.append({
                "width": int(adv),
                "height": 0,
                "x_offset": 0,
                "y_offset": 0,
                "bitmap_idx": bitmap_idx
            })
            continue
            
        left, top, right, bottom = bbox
        g_width = right - left
        g_height = bottom - top
        
        cropped = img.crop((left, top, right, bottom))
        pixels = list(cropped.getdata())
        
        packed_bytes = bytearray()
        current_byte = 0
        bit_pos = 0
        
        for p in pixels:
            if p > 0:
                current_byte |= (0x80 >> bit_pos)
            bit_pos += 1
            if bit_pos == 8:
                packed_bytes.append(current_byte)
                current_byte = 0
                bit_pos = 0
                
        if bit_pos > 0:
            packed_bytes.append(current_byte)
            
        x_offset = left - size
        y_offset = top - size
        
        glyphs.append({
            "width": g_width,
            "height": g_height,
            "x_offset": x_offset,
            "y_offset": y_offset,
            "bitmap_idx": bitmap_idx
        })
        
        bitmap_data.extend(packed_bytes)
        bitmap_idx += len(packed_bytes)

    return {
        "suffix": name_suffix,
        "line_height": line_height,
        "glyphs": glyphs,
        "bitmap": bitmap_data,
        "start_char": 32,
        "end_char": 126
    }

def generate_cpp(fonts, out_cpp, out_hpp):
    hpp_content = [
        "#ifndef AURORA_FONT_DATA_HPP",
        "#define AURORA_FONT_DATA_HPP",
        "",
        '#include "font_engine.hpp"',
        ""
    ]
    
    cpp_content = [
        '#include "font_data.hpp"',
        ""
    ]
    
    for f in fonts:
        s = f["suffix"]
        
        cpp_content.append(f"const uint8_t font_bitmap_{s}[] = {{")
        line = "    "
        for i, b in enumerate(f["bitmap"]):
            line += f"0x{b:02X}, "
            if (i + 1) % 16 == 0:
                cpp_content.append(line)
                line = "    "
        if line != "    ":
            cpp_content.append(line)
        cpp_content.append("};")
        cpp_content.append("")
        
        cpp_content.append(f"const GlyphInfo font_glyphs_{s}[] = {{")
        for g in f["glyphs"]:
            cpp_content.append(f"    {{{g['width']}, {g['height']}, {g['x_offset']}, {g['y_offset']}, {g['bitmap_idx']}}},")
        cpp_content.append("};")
        cpp_content.append("")
        
        hpp_content.append(f"extern const FontDef font_{s};")
        
        cpp_content.append(f"const FontDef font_{s} = {{")
        cpp_content.append(f"    font_bitmap_{s},")
        cpp_content.append(f"    font_glyphs_{s},")
        cpp_content.append(f"    {f['line_height']},")
        cpp_content.append(f"    {f['start_char']},")
        cpp_content.append(f"    {f['end_char']}")
        cpp_content.append("};")
        cpp_content.append("")
        
    hpp_content.append("#endif // AURORA_FONT_DATA_HPP\n")
    
    os.makedirs(os.path.dirname(out_cpp), exist_ok=True)
    with open(out_cpp, "w") as f:
        f.write("\n".join(cpp_content))
    with open(out_hpp, "w") as f:
        f.write("\n".join(hpp_content))

if __name__ == "__main__":
    font_path = "C:/Windows/Fonts/arial.ttf"
    fonts = []
    
    f_medium = generate_font(font_path, 20, "medium")
    if f_medium:
        fonts.append(f_medium)
        
    f_huge = generate_font(font_path, 40, "huge")
    if f_huge:
        fonts.append(f_huge)
        
    out_cpp = "j:/PROJECT/auroraOS/apps/watch/font_data.cpp"
    out_hpp = "j:/PROJECT/auroraOS/apps/watch/font_data.hpp"
    generate_cpp(fonts, out_cpp, out_hpp)
    print("Font data generated successfully.")
