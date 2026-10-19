file(READ "${INPUT}" _style HEX)
string(REGEX REPLACE "(..)" "0x\\1," _style "${_style}")
file(WRITE "${OUTPUT}" "/* Generated from default_style.toml; no runtime file dependency. */\nstatic const unsigned char de_ui_default_style[] = {${_style}0};\n")
