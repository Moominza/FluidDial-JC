# JC pendant fonts

The JC touchscreen UI uses Roboto and Roboto Mono from the Google Fonts
repository. The original variable font files and their SIL Open Font License
1.1 texts are retained here:

- `Roboto-Variable.ttf` and `Roboto-OFL.txt`
- `RobotoMono-Variable.ttf` and `RobotoMono-OFL.txt`

The UI embeds fixed-size 4-bit grayscale glyph rasters generated from these
files by `tools/generate_jc_fonts.py`. Roboto Mono supplies equal-width,
tabular coordinate digits. No runtime font engine or scaled bitmap font is
used. The generated C++ data is checked in at
`src/jc8048w550/JcFonts.generated.hpp`.

Regenerate with Python 3.10 or later and Pillow installed:

```powershell
python tools\generate_jc_fonts.py
```

The font subsets and their generated raster data remain subject to the
included SIL Open Font License files.
